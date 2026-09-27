#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/mm2-ablation-medium
SYNCBASE=/shared/vine-audit/mm2-libptu-sync-medium
WFDIR=/shared/libptu/dataset/minimap2_sv/workflow
LOCALCWD=/tmp/mm2-sync-local-test
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

MGR_ARGS="--reference-dir /shared/minimap2-sv-data-medium/reference --data-dir /shared/minimap2-sv-data-medium/data --output-dir output --cores-per-task 2 --ports $PORT 9150"

# Conditions 1-4: run in place from the permanent workflow dir.
run_condition "base-execution" /shared/vine-audit/mm2_workers_plain.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && python -u minimap2_benchmark.py --scheduler taskvine --name mm2-base-medium $MGR_ARGS" \
  "$BASE"

run_condition "base-audit-ptrace" /shared/vine-audit/mm2_workers_ptu.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && python -u minimap2_benchmark.py --scheduler taskvine --name mm2-baseaudit-medium $MGR_ARGS" \
  "$BASE"

run_condition "full-fidelis" /shared/vine-audit/mm2_workers_fidelis.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output $BASE/manager-fidelis python -u minimap2_benchmark.py --scheduler taskvine --name mm2-fidelis-medium $MGR_ARGS" \
  "$BASE"

run_condition "ptrace-reuse" /shared/vine-audit/mm2_workers_ptu.sbatch \
  "cd $WFDIR && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && TASKVINE_WARM_POOL=1 python -u minimap2_benchmark.py --scheduler taskvine --name mm2-reuse-medium $MGR_ARGS" \
  "$BASE"

# Conditions 5-6: run from a clean local CWD (matches libptu-sync driver convention).
SYNC_MGR_ARGS="--reference-dir /shared/minimap2-sv-data-medium/reference --data-dir /shared/minimap2-sv-data-medium/data --output-dir output --cores-per-task 2 --ports $PORT 9150"

run_condition "interposition-only-sync" /shared/vine-audit/mm2_workers_libptu_sync.sbatch \
  "cd $LOCALCWD && rm -rf vine-run-info __pycache__ output && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $SYNCBASE/manager-interp-only python -u $WFDIR/minimap2_benchmark.py --scheduler taskvine --name mm2-interp-only-sync-medium $SYNC_MGR_ARGS" \
  "$SYNCBASE"

run_condition "interposition-reuse-sync" /shared/vine-audit/mm2_workers_libptu_sync.sbatch \
  "cd $LOCALCWD && rm -rf vine-run-info __pycache__ output && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $SYNCBASE/manager-interp-reuse python -u $WFDIR/minimap2_benchmark.py --scheduler taskvine --name mm2-interp-reuse-sync-medium $SYNC_MGR_ARGS" \
  "$SYNCBASE"

echo "[driver] $(date) ALL 6 CONDITIONS DONE"
