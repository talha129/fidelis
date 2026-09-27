#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dconv-local-staging-small
OUTBASE=/shared/dconv-output/local-staging-small
WFDIR=/shared/libptu/dataset/distributed_image_convolution/workflow
IMG=$WFDIR/npp.jpg
LOCALCWD=/tmp/dconv-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD" "$OUTBASE"

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

  echo "[driver] $(date) $name: starting manager (CWD=$LOCALCWD, wall-clock cap 20700s)..."
  local MLOG="$BASE/manager_${name}.log"
  ( cd "$LOCALCWD" && timeout -k 60 20700 bash -c "$manager_cmd" ) 2>&1 | tee "$MLOG"
  local RC=${PIPESTATUS[0]}
  if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "[driver] $name: TIMEOUT after 20700s wall-clock cap (workers likely died / SLURM job expired) RC=$RC"
  elif [ "$RC" -ne 0 ]; then
    echo "[driver] $name: manager exited nonzero RC=$RC"
  fi

  echo "[driver] $(date) $name: manager finished (RC=$RC). Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--images $IMG --kernels sharpen --tile-size 256"

run_condition "base-execution" /shared/vine-audit/dconv_workers_plain_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && python -u $WFDIR/image_convolution_benchmark.py --name dconv-base-small --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/base-execution"
run_condition "base-audit-ptrace" /shared/vine-audit/dconv_workers_ptu_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && python -u $WFDIR/image_convolution_benchmark.py --name dconv-baseaudit-small --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/base-audit-ptrace"
run_condition "full-fidelis" /shared/vine-audit/dconv_workers_fidelis_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u $WFDIR/image_convolution_benchmark.py --name dconv-fidelis-small --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/full-fidelis"
run_condition "ptrace-reuse" /shared/vine-audit/dconv_workers_ptu_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate dconv && TASKVINE_WARM_POOL=1 python -u $WFDIR/image_convolution_benchmark.py --name dconv-reuse-small --ports $PORT 9150 $MGR_ARGS --output-dir $OUTBASE/ptrace-reuse"


echo "[driver] $(date) ALL CONDITIONS DONE"
