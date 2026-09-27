#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/gatk-ablation-medium
SYNCBASE=/shared/vine-audit/gatk-libptu-sync-medium
WFDIR=/shared/libptu/dataset/gatk_hc/workflow
LOCALCWD=/tmp/gatk-sync-local-test
PORT=9123
mkdir -p "$BASE" "$SYNCBASE" "$LOCALCWD" "$WFDIR/output"

run_condition() {
  local name="$1"
  local sbatch_file="$2"
  local manager_cmd="$3"
  local log_base="$4"

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

  echo "[driver] $(date) $name: starting manager (wall-clock cap 2400s)..."
  local MLOG="${log_base}/manager_${name}.log"
  ( eval "timeout -k 60 2400 bash -c \"$manager_cmd\"" ) 2>&1 | tee "$MLOG"
  local RC=${PIPESTATUS[0]}
  if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "[driver] $name: TIMEOUT after 2400s wall-clock cap RC=$RC"
  elif [ "$RC" -ne 0 ]; then
    echo "[driver] $name: manager exited nonzero RC=$RC"
  fi

  echo "[driver] $(date) $name: manager finished (RC=$RC). Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

DATA=/shared/gatk-hc-data-medium
MGR_ARGS="--reference-dir $DATA/reference --bam-dir $DATA/bam --intervals-dir $DATA/intervals --output-dir output --cores-per-task 2 --heap 2g --ports $PORT 9150"

run_condition "base-execution" /shared/vine-audit/gatk_workers_plain.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && python -u gatk_benchmark.py --scheduler taskvine --name gatk-base-medium $MGR_ARGS" \
  "$BASE"

run_condition "base-audit-ptrace" /shared/vine-audit/gatk_workers_ptu.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && python -u gatk_benchmark.py --scheduler taskvine --name gatk-baseaudit-medium $MGR_ARGS" \
  "$BASE"

run_condition "full-fidelis" /shared/vine-audit/gatk_workers_fidelis.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u gatk_benchmark.py --scheduler taskvine --name gatk-fidelis-medium $MGR_ARGS" \
  "$BASE"

run_condition "ptrace-reuse" /shared/vine-audit/gatk_workers_ptu.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && TASKVINE_WARM_POOL=1 python -u gatk_benchmark.py --scheduler taskvine --name gatk-reuse-medium $MGR_ARGS" \
  "$BASE"

SYNC_MGR_ARGS="--reference-dir $DATA/reference --bam-dir $DATA/bam --intervals-dir $DATA/intervals --output-dir output --cores-per-task 2 --heap 2g --ports $PORT 9150"

run_condition "interposition-only-sync" /shared/vine-audit/gatk_workers_libptu_sync.sbatch \
  "cd $LOCALCWD && rm -rf vine-run-info __pycache__ output && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $SYNCBASE/manager-interp-only python -u $WFDIR/gatk_benchmark.py --scheduler taskvine --name gatk-interp-only-sync-medium $SYNC_MGR_ARGS" \
  "$SYNCBASE"

run_condition "interposition-reuse-sync" /shared/vine-audit/gatk_workers_libptu_sync.sbatch \
  "cd $LOCALCWD && rm -rf vine-run-info __pycache__ output && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $SYNCBASE/manager-interp-reuse python -u $WFDIR/gatk_benchmark.py --scheduler taskvine --name gatk-interp-reuse-sync-medium $SYNC_MGR_ARGS" \
  "$SYNCBASE"

echo "[driver] $(date) ALL 6 CONDITIONS DONE"
