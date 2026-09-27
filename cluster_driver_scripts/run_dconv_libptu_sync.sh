#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dconv-libptu-sync-medium
OUTBASE=/shared/dconv-output/libptu-sync-medium
LOCALCWD=/tmp/dconv-local-test
WFDIR=/shared/libptu/dataset/distributed_image_convolution/workflow
IMG=$WFDIR/npp.jpg
PORT=9123
mkdir -p "$BASE" "$OUTBASE" "$LOCALCWD"

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
  ( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ output/* && eval "$manager_cmd" ) 2>&1 | tee "$MLOG"

  echo "[driver] $(date) $name: manager finished. Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--images $IMG $IMG $IMG $IMG $IMG --kernels sharpen --tile-size 256"

run_condition "interposition-only-sync" /shared/vine-audit/dconv_workers_libptu_sync.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-only python -u $WFDIR/image_convolution_benchmark.py --name dconv-interp-only-sync --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/interp-only"

run_condition "interposition-reuse-sync" /shared/vine-audit/dconv_workers_libptu_sync.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-reuse python -u $WFDIR/image_convolution_benchmark.py --name dconv-interp-reuse-sync --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/interp-reuse"

echo "[driver] $(date) ALL CONDITIONS DONE"
