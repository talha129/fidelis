#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/ctrend-ablation-medium
mkdir -p "$BASE"
WFDIR=/shared/libptu/dataset/climate_trend/workflow
PORT=9123

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
      echo "[driver] $name: job running, giving workers 25s to connect..."
      sleep 25
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

  echo "[driver] $(date) $name: starting manager..."
  local MLOG="$BASE/manager_${name}.log"
  ( cd "$WFDIR" && eval "$manager_cmd" ) 2>&1 | tee "$MLOG"

  echo "[driver] $(date) $name: manager finished. Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS='--num-files 1500 --data-source data/csv_index.json --output-dir /shared/ctrend-data'

run_condition "base-execution" /shared/vine-audit/ctrend_workers_plain.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && python climate_trend_benchmark.py --name ctrend-base --ports $PORT 9150 $MGR_ARGS"

run_condition "base-audit-ptrace" /shared/vine-audit/ctrend_workers_ptu.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && python climate_trend_benchmark.py --name ctrend-baseaudit --ports $PORT 9150 $MGR_ARGS"

run_condition "full-fidelis" /shared/vine-audit/ctrend_workers_fidelis.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python climate_trend_benchmark.py --name ctrend-fidelis --ports $PORT 9150 $MGR_ARGS"

run_condition "ptrace-reuse" /shared/vine-audit/ctrend_workers_ptu.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate ctrend && TASKVINE_WARM_POOL=1 python climate_trend_benchmark.py --name ctrend-reuse --ports $PORT 9150 $MGR_ARGS"

echo "[driver] $(date) ALL CONDITIONS DONE"
