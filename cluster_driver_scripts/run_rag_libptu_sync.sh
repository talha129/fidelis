#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/rag-libptu-sync-medium
OUTDIR=/shared/rag-output/libptu-sync-medium
LOCALCWD=/tmp/rag-local-test
DATADIR=/shared/rag-data
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
  ( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ rag-staging && eval "$manager_cmd" ) 2>&1 | tee "$MLOG"

  echo "[driver] $(date) $name: manager finished. Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--data-dir $DATADIR --num-files 2500 --skip-query --staging-path $LOCALCWD/rag-staging"

run_condition "interposition-only-sync" /shared/vine-audit/rag_workers_libptu_sync.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-only python -u rag_benchmark.py --name rag-interp-only-sync --ports $PORT 9150 $MGR_ARGS --output $OUTDIR/interp-only.json"

run_condition "interposition-reuse-sync" /shared/vine-audit/rag_workers_libptu_sync.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-reuse python -u rag_benchmark.py --name rag-interp-reuse-sync --ports $PORT 9150 $MGR_ARGS --output $OUTDIR/interp-reuse.json"

echo "[driver] $(date) ALL CONDITIONS DONE"
