#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/debug-warmpool-only-fullscale
WFDIR=/shared/libptu/dataset/dask-taskvine-mapreduce-benchmark/workflow

echo "[debug-driver] $(date +%s.%N) Submitting worker job (plain, unwrapped)..."
JOBID=$(sbatch /shared/vine-audit/mr_workers_plain_debug.sbatch | awk '{print $4}')
echo "[debug-driver] Submitted job $JOBID"

for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[debug-driver] $(date +%s.%N) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    echo "[debug-driver] $(date +%s.%N) job RUNNING -- giving 25s connect buffer"
    sleep 25
    break
  fi
  if [ -z "$STATE" ]; then
    echo "[debug-driver] ERROR job disappeared"; exit 1
  fi
  sleep 10
done

echo "[debug-driver] $(date +%s.%N) MANAGER_LAUNCH_START"
cd "$WFDIR"
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate map-reduce
TASKVINE_WARM_POOL=1 python -u mapreduce_benchmark.py --scheduler taskvine --port 9123 --input-dir /shared/map-reduce-data --num-files 4096 2>&1 | while IFS= read -r line; do echo "$(date +%s.%N) | $line"; done | tee $BASE/manager_debug.log

echo "[debug-driver] $(date +%s.%N) MANAGER_LAUNCH_END"
scancel "$JOBID"
echo "[debug-driver] $(date +%s.%N) DONE"
