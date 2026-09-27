#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dv5-local-staging-medium
OUTDIR=/shared/dv5-output/local-staging-medium
LOCALCWD=/tmp/dv5-local-test
SAMPLES_READY=/shared/dv5-input-samples/samples_ready_medium20.json
PORT=9123
mkdir -p "$BASE" "$OUTDIR" "$LOCALCWD"

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
  ( cd "$LOCALCWD" && eval "$manager_cmd" ) 2>&1 | tee "$MLOG"

  echo "[driver] $(date) $name: manager finished. Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--data-dir /shared/dv5-input-samples --sub-dataset hgg_1 --num-files 20 --samples-ready $SAMPLES_READY --triggers-file $LOCALCWD/triggers.json --staging-path $LOCALCWD/dv5-staging"

run_condition "base-execution" /shared/vine-audit/dv5_workers_plain.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && python -u dv5_benchmark.py --name dv5-base-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTDIR/base-execution"

run_condition "base-audit-ptrace" /shared/vine-audit/dv5_workers_ptu.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && python -u dv5_benchmark.py --name dv5-baseaudit-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTDIR/base-audit-ptrace"

run_condition "full-fidelis" /shared/vine-audit/dv5_workers_fidelis.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u dv5_benchmark.py --name dv5-fidelis-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTDIR/full-fidelis"

run_condition "ptrace-reuse" /shared/vine-audit/dv5_workers_ptu.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && TASKVINE_WARM_POOL=1 python -u dv5_benchmark.py --name dv5-reuse-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTDIR/ptrace-reuse"

echo "[driver] $(date) ALL CONDITIONS DONE"
