#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dv5-local-staging-small
LOCALCWD=/tmp/dv5-local-test
mkdir -p "$BASE" "$LOCALCWD"

echo "[prep-small] $(date) submitting worker job..."
JOBID=$(sbatch /shared/vine-audit/dv5_workers_plain_small.sbatch | awk '{print $4}')
echo "[prep-small] submitted worker job $JOBID"

STATE=""
for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[prep-small] $(date) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    echo "[prep-small] job running, waiting for worker connection buffer..."
    break
  fi
  if [ -z "$STATE" ]; then
    echo "[prep-small] ERROR job disappeared before running"
    exit 1
  fi
  sleep 10
done
if [ "$STATE" != "RUNNING" ]; then
  echo "[prep-small] ERROR worker job never reached RUNNING (state=$STATE)"
  exit 1
fi
sleep 30

echo "[prep-small] $(date) starting preprocessing run (throwaway, not timed)..."
( cd "$LOCALCWD" && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && \
  python -u dv5_benchmark.py --name dv5-prep-small --ports 9123 9150 \
  --data-dir /shared/dv5-input-samples --preprocess --sub-dataset hgg_1 --num-files 4 \
  --samples-ready /shared/dv5-input-samples/samples_ready_small4.json \
  --triggers-file $LOCALCWD/triggers.json --staging-path $LOCALCWD/dv5-staging \
  --output-dir $LOCALCWD/output-prep-small ) 2>&1 | tee "$BASE/prep.log"

echo "[prep-small] $(date) prep run finished. Cancelling worker job $JOBID..."
scancel "$JOBID"
sleep 5
echo "[prep-small] $(date) PREP DONE"
