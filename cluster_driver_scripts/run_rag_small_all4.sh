#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/rag-local-staging-small
OUTDIR=/shared/rag-output/local-staging-small
DATADIR=/shared/rag-data
LOCALCWD=/tmp/rag-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD" "$OUTDIR"

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

  echo "[driver] $(date) $name: starting manager (CWD=$LOCALCWD, wall-clock cap 20700s)..."
  local MLOG="$BASE/manager_${name}.log"
  ( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ rag-staging && timeout -k 60 20700 bash -c "$manager_cmd" ) 2>&1 | tee "$MLOG"
  local RC=${PIPESTATUS[0]}
  if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "[driver] $name: TIMEOUT after 20700s wall-clock cap (workers likely died / SLURM job expired) RC=$RC"
  elif [ "$RC" -ne 0 ]; then
    echo "[driver] $name: manager exited nonzero RC=$RC"
  fi

  echo "[driver] $(date) $name: manager finished (RC=$RC). Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--data-dir $DATADIR --num-files 500 --skip-query --staging-path $LOCALCWD/rag-staging"

run_condition "base-execution" /shared/vine-audit/rag_workers_plain_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && python -u rag_benchmark.py --name rag-base-small --ports $PORT 9150 $MGR_ARGS --output $OUTDIR/base-execution.json"
run_condition "base-audit-ptrace" /shared/vine-audit/rag_workers_ptu_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && python -u rag_benchmark.py --name rag-baseaudit-small --ports $PORT 9150 $MGR_ARGS --output $OUTDIR/base-audit-ptrace.json"
run_condition "full-fidelis" /shared/vine-audit/rag_workers_fidelis_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u rag_benchmark.py --name rag-fidelis-small --ports $PORT 9150 $MGR_ARGS --output $OUTDIR/full-fidelis.json"
run_condition "ptrace-reuse" /shared/vine-audit/rag_workers_ptu_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && TASKVINE_WARM_POOL=1 python -u rag_benchmark.py --name rag-reuse-small --ports $PORT 9150 $MGR_ARGS --output $OUTDIR/ptrace-reuse.json"


echo "[driver] $(date) ALL CONDITIONS DONE"
