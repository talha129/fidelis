#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/rag-local-staging-medium
OUTDIR=/shared/rag-output/local-staging-medium
LOCALCWD=/tmp/rag-local-test
DATADIR=/shared/rag-data
PORT=9123

echo "[driver] $(date) ========== Condition: base-execution-rerun =========="
JOBID=$(sbatch /shared/vine-audit/rag_workers_plain.sbatch | awk '{print $4}')
echo "[driver] submitted worker job $JOBID"

STATE=""
for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[driver] $(date) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    echo "[driver] job running, waiting for worker connection buffer..."
    break
  fi
  if [ -z "$STATE" ]; then
    echo "[driver] ERROR job disappeared before running"
    scontrol show job "$JOBID" 2>&1 || true
    exit 1
  fi
  sleep 10
done
if [ "$STATE" != "RUNNING" ]; then
  echo "[driver] ERROR worker job never reached RUNNING (state=$STATE)"
  exit 1
fi
sleep 30

echo "[driver] $(date) starting manager (CWD=$LOCALCWD)..."
MLOG="$BASE/manager_base-execution-rerun.log"
( cd "$LOCALCWD" && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate rag && \
  python -u rag_benchmark.py --name rag-base-rerun --ports $PORT 9150 \
  --data-dir $DATADIR --num-files 2500 --skip-query --staging-path $LOCALCWD/rag-staging \
  --output $OUTDIR/base-execution-rerun.json ) 2>&1 | tee "$MLOG"

echo "[driver] $(date) manager finished. Cancelling worker job $JOBID..."
scancel "$JOBID"
sleep 5
echo "[driver] $(date) ========== base-execution-rerun DONE. log=$MLOG =========="
