#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dv5-local-staging-large
OUTDIR=/shared/dv5-output/local-staging-large
LOCALCWD=/tmp/dv5-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD"

JOBID=$(sbatch /shared/vine-audit/dv5_workers_plain_large.sbatch | awk '{print $4}')
echo "[rerun] submitted worker job $JOBID"
for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[rerun] $(date) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    break
  fi
  if [ -z "$STATE" ]; then
    echo ERROR
    exit 1
  fi
  sleep 10
done
sleep 30

MGR_CMD="python -u dv5_benchmark.py --name dv5-base-large-rerun --ports $PORT 9150 --data-dir /shared/dv5-input-samples --sub-dataset hgg_1 --num-files 60 --samples-ready /shared/dv5-input-samples/samples_ready_large60.json --triggers-file $LOCALCWD/triggers.json --staging-path $LOCALCWD/dv5-staging --output-dir $OUTDIR/base-execution-rerun"

MLOG="$BASE/manager_base-execution-rerun.log"
( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ dv5-staging && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dv5 && timeout -k 60 20700 bash -c "$MGR_CMD" ) 2>&1 | tee "$MLOG"

cp -r /tmp/dv5-local-test/vine-run-info/most-recent /shared/vine-audit/dv5-large-vineinfo-backups/base-execution-rerun
scancel "$JOBID"
echo "[rerun] DONE"
