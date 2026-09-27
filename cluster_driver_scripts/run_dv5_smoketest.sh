#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dv5-local-staging-medium
OUTDIR=/shared/dv5-output/local-staging-medium
LOCALCWD=/tmp/dv5-local-test
SAMPLES_READY=/shared/dv5-input-samples/samples_ready_medium20.json
PORT=9123
mkdir -p "$BASE" "$OUTDIR" "$LOCALCWD"

echo "[smoke] $(date) submitting worker job..."
JOBID=$(sbatch /shared/vine-audit/dv5_workers_plain.sbatch | awk '{print $4}')
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
( cd "$LOCALCWD" && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && \
  python -u dv5_benchmark.py --name dv5-base-smoke --ports $PORT 9150 \
  --data-dir /shared/dv5-input-samples --sub-dataset hgg_1 --num-files 20 \
  --samples-ready $SAMPLES_READY --triggers-file $LOCALCWD/triggers.json \
  --staging-path $LOCALCWD/dv5-staging --output-dir $OUTDIR/base-execution ) \
  2>&1 | tee "$BASE/manager_base-execution.log"

echo "[smoke] $(date) manager finished. Cancelling worker job $JOBID..."
scancel "$JOBID"
sleep 5
echo "[smoke] $(date) SMOKETEST DONE"
