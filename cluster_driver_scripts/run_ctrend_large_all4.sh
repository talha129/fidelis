#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/ctrend-local-staging-large
OUTDIR=/shared/ctrend-data-local-staging-large
WFDIR=/shared/libptu/dataset/climate_trend/workflow
DATASRC=$WFDIR/data/csv_index.json
LOCALCWD=/tmp/ctrend-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD" "$OUTDIR"

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

MGR_ARGS="--num-files 3000 --data-source $DATASRC --output-dir $OUTDIR"

run_condition "base-execution" /shared/vine-audit/ctrend_workers_plain_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && python -u $WFDIR/climate_trend_benchmark.py --name ctrend-base-large --ports $PORT 9150 $MGR_ARGS"
run_condition "base-audit-ptrace" /shared/vine-audit/ctrend_workers_ptu_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && python -u $WFDIR/climate_trend_benchmark.py --name ctrend-baseaudit-large --ports $PORT 9150 $MGR_ARGS"
run_condition "full-fidelis" /shared/vine-audit/ctrend_workers_fidelis_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u $WFDIR/climate_trend_benchmark.py --name ctrend-fidelis-large --ports $PORT 9150 $MGR_ARGS"
run_condition "ptrace-reuse" /shared/vine-audit/ctrend_workers_ptu_large.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && TASKVINE_WARM_POOL=1 python -u $WFDIR/climate_trend_benchmark.py --name ctrend-reuse-large --ports $PORT 9150 $MGR_ARGS"


echo "[driver] $(date) ALL CONDITIONS DONE"
