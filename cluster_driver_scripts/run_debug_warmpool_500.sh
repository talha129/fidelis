#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/debug-warmpool-500
WFDIR=/shared/libptu/dataset/dask-taskvine-mapreduce-benchmark/workflow

JOBID=$(sbatch /shared/vine-audit/mr_workers_plain_debug500.sbatch | awk '{print $4}')
echo "[debug-driver] Submitted job $JOBID"

for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  if [ "$STATE" = "RUNNING" ]; then sleep 15; break; fi
  if [ -z "$STATE" ]; then echo "[debug-driver] ERROR job disappeared"; exit 1; fi
  sleep 8
done

echo "[debug-driver] MANAGER_LAUNCH_START $(date +%s.%N)"
cd "$WFDIR"
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate map-reduce
TASKVINE_WARM_POOL=1 TASKVINE_WARM_DEBUG=1 python -u mapreduce_benchmark.py --scheduler taskvine --port 9123 --input-dir /shared/map-reduce-data --num-files 500 2>&1 | tee $BASE/manager_debug.log

scancel "$JOBID"
echo "[debug-driver] DONE"
