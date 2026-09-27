#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/gatk-libptu-sync-medium
WFDIR=/shared/libptu/dataset/gatk_hc/workflow
LOCALCWD=/tmp/gatk-sync-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD"

run_condition() {
  local name="$1"; local sbatch_file="$2"; local manager_cmd="$3"
  echo "[driver] $(date) ========== Condition: $name =========="
  local JOBID; JOBID=$(sbatch "$sbatch_file" | awk '{print $4}')
  echo "[driver] $name: submitted worker job $JOBID"
  local STATE=""
  for i in $(seq 1 60); do
    STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
    if [ "$STATE" = "RUNNING" ]; then break; fi
    if [ -z "$STATE" ]; then echo "[driver] $name: ERROR job disappeared"; return 1; fi
    sleep 10
  done
  if [ "$STATE" != "RUNNING" ]; then echo "[driver] $name: ERROR never RUNNING (state=$STATE)"; return 1; fi
  sleep 30
  local MLOG="$BASE/manager_${name}.log"
  ( eval "timeout -k 60 2400 bash -c \"$manager_cmd\"" ) 2>&1 | tee "$MLOG"
  local RC=${PIPESTATUS[0]}
  echo "[driver] $name: manager finished RC=$RC. Cancelling worker job $JOBID..."
  scancel "$JOBID"; sleep 5
  echo "[driver] $(date) ========== $name DONE =========="
}

DATA=/shared/gatk-hc-data-medium
SYNC_MGR_ARGS="--reference-dir $DATA/reference --bam-dir $DATA/bam --intervals-dir $DATA/intervals --output-dir output --cores-per-task 2 --heap 2g --ports $PORT 9150"

run_condition "interposition-only-sync" /shared/vine-audit/gatk_workers_libptu_sync.sbatch   "cd $LOCALCWD && rm -rf vine-run-info __pycache__ output && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-only python -u $WFDIR/gatk_benchmark.py --scheduler taskvine --name gatk-interp-only-sync-medium $SYNC_MGR_ARGS"

run_condition "interposition-reuse-sync" /shared/vine-audit/gatk_workers_libptu_sync.sbatch   "cd $LOCALCWD && rm -rf vine-run-info __pycache__ output && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-reuse python -u $WFDIR/gatk_benchmark.py --scheduler taskvine --name gatk-interp-reuse-sync-medium $SYNC_MGR_ARGS"

echo "[driver] ALL REMAINING CONDITIONS DONE"
