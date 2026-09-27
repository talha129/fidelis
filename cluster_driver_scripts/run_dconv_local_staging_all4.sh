#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dconv-local-staging-medium
OUTBASE=/shared/dconv-output/local-staging-medium
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
  ( cd "$LOCALCWD" && eval "$manager_cmd" ) 2>&1 | tee "$MLOG"

  echo "[driver] $(date) $name: manager finished. Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--images $IMG $IMG $IMG $IMG $IMG --kernels sharpen --tile-size 256"

run_condition "base-execution" /shared/vine-audit/dconv_workers_plain.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && python -u $WFDIR/image_convolution_benchmark.py --name dconv-base-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/base-execution"

run_condition "base-audit-ptrace" /shared/vine-audit/dconv_workers_ptu.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && python -u $WFDIR/image_convolution_benchmark.py --name dconv-baseaudit-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/base-audit-ptrace"

run_condition "full-fidelis" /shared/vine-audit/dconv_workers_fidelis.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u $WFDIR/image_convolution_benchmark.py --name dconv-fidelis-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/full-fidelis"

run_condition "ptrace-reuse" /shared/vine-audit/dconv_workers_ptu.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && TASKVINE_WARM_POOL=1 python -u $WFDIR/image_convolution_benchmark.py --name dconv-reuse-ls --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/ptrace-reuse"

echo "[driver] $(date) ALL CONDITIONS DONE"
