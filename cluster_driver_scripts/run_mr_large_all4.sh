#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/map-reduce-local-staging-large

LOCALCWD=/tmp/mr-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD"

run_condition() {
  local name="$1"
  local sbatch_file="$2"
  local manager_cmd="$3"

  echo "[driver] $(date) ========== Condition: $name =========="
  local JOBID
  JOBID=$(sbatch "$sbatch_file" | awk '{print $4}')
  echo "[driver] $name: submitted worker job $JOBID"

  local STATE=""
  for i in $(seq 1 60); do
    STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
    echo "[driver] $(date) $name poll $i: state=${STATE:-GONE}"
    if [ "$STATE" = "RUNNING" ]; then
      echo "[driver] $name: job running, waiting for worker connection buffer..."
      break
    fi
    if [ -z "$STATE" ]; then
      echo "[driver] $name: ERROR job disappeared before running"
      scontrol show job "$JOBID" 2>&1 || true
      return 1
    fi
    sleep 10
  done
  if [ "$STATE" != "RUNNING" ]; then
    echo "[driver] $name: ERROR worker job never reached RUNNING (state=$STATE)"
    return 1
  fi
  sleep 30

  echo "[driver] $(date) $name: starting manager (CWD=$LOCALCWD)..."
  local MLOG="$BASE/manager_${name}.log"
  ( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ results && eval "$manager_cmd" ) 2>&1 | tee "$MLOG"

  echo "[driver] $(date) $name: manager finished. Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--scheduler taskvine --port $PORT --input-dir /shared/map-reduce-data --num-files 16384 --combine-width 2"

run_condition "base-execution" /shared/vine-audit/mr_workers_plain_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate map-reduce && python -u mapreduce_benchmark.py --name mr-base-large $MGR_ARGS"
run_condition "base-audit-ptrace" /shared/vine-audit/mr_workers_ptu_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate map-reduce && python -u mapreduce_benchmark.py --name mr-baseaudit-large $MGR_ARGS"
run_condition "full-fidelis" /shared/vine-audit/mr_workers_fidelis_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate map-reduce && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u mapreduce_benchmark.py --name mr-fidelis-large $MGR_ARGS"
run_condition "ptrace-reuse" /shared/vine-audit/mr_workers_ptu_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate map-reduce && TASKVINE_WARM_POOL=1 python -u mapreduce_benchmark.py --name mr-reuse-large $MGR_ARGS"


echo "[driver] $(date) ALL CONDITIONS DONE"
