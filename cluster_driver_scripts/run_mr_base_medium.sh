#!/bin/bash
set -e
export PATH=/opt/slurm/bin:$PATH
LOGDIR=/shared/vine-audit/map-reduce-base-medium
mkdir -p "$LOGDIR"
RUNID=$(date +%Y%m%d_%H%M%S)
PORT=9123

echo "[driver] $(date) Submitting worker job (8 nodes, bash sbatch script)..."
JOBID=$(sbatch /shared/vine-audit/mr_workers.sbatch | awk '{print $4}')
echo "[driver] Submitted job $JOBID"

echo "[driver] Waiting for job to reach RUNNING..."
for i in $(seq 1 40); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[driver] $(date) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    echo "[driver] Job running. Giving workers 20s to connect..."
    sleep 20
    break
  fi
  if [ -z "$STATE" ]; then
    echo "[driver] Job disappeared from queue before running - check scontrol/output file."
    scontrol show job "$JOBID" 2>&1 || true
    exit 1
  fi
  sleep 8
done

FINAL_STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
if [ "$FINAL_STATE" != "RUNNING" ]; then
  echo "[driver] ERROR: worker job never reached RUNNING (state=$FINAL_STATE). Aborting."
  exit 1
fi

echo "[driver] $(date) Starting manager: Base Execution, MapReduce, medium scale (--num-files 4096)..."
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate map-reduce
cd /shared/libptu/dataset/dask-taskvine-mapreduce-benchmark/workflow
MANAGER_LOG="$LOGDIR/manager_${RUNID}.log"
python mapreduce_benchmark.py --scheduler taskvine --port $PORT --input-dir /shared/map-reduce-data --num-files 4096 2>&1 | tee "$MANAGER_LOG"

echo "[driver] $(date) Manager finished. Cancelling worker job $JOBID to release nodes..."
scancel "$JOBID"
echo "[driver] $(date) DONE. Log at $MANAGER_LOG"
