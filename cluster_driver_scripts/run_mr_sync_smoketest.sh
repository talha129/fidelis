#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/map-reduce-libptu-sync-medium
LOCALCWD=/tmp/mr-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD"

echo "[smoke] $(date) submitting worker job..."
JOBID=$(sbatch /shared/vine-audit/mr_workers_libptu_sync.sbatch | awk '{print $4}')
echo "[smoke] submitted worker job $JOBID"

STATE=""
for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[smoke] $(date) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    echo "[smoke] job running, waiting for worker connection buffer..."
    break
  fi
  if [ -z "$STATE" ]; then
    echo "[smoke] ERROR job disappeared before running"
    exit 1
  fi
  sleep 10
done
if [ "$STATE" != "RUNNING" ]; then
  echo "[smoke] ERROR worker job never reached RUNNING (state=$STATE)"
  exit 1
fi
sleep 30

echo "[smoke] $(date) starting manager (CWD=$LOCALCWD)..."
( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ results && \
  source /shared/miniconda3/etc/profile.d/conda.sh && conda activate map-reduce && \
  VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-smoketest \
  python -u mapreduce_benchmark.py --scheduler taskvine --port $PORT --name mr-smoketest-sync \
  --input-dir /shared/map-reduce-data --num-files 20 --combine-width 2 ) \
  2>&1 | tee "$BASE/manager_smoketest.log"

echo "[smoke] $(date) manager finished. Cancelling worker job $JOBID..."
scancel "$JOBID"
sleep 5
echo "[smoke] $(date) SMOKETEST DONE"
