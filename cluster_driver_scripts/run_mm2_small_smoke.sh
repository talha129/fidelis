#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/minimap2-sv-small

LOCALCWD=/tmp/mm2-local-test
PORT=9123
mkdir -p "$BASE" "$LOCALCWD"

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

  echo "[driver] $(date) $name: starting manager (CWD=$LOCALCWD, wall-clock cap 1200s)..."
  local MLOG="$BASE/manager_${name}.log"
  ( cd "$LOCALCWD" && rm -rf vine-run-info __pycache__ output && timeout -k 60 1200 bash -c "$manager_cmd" ) 2>&1 | tee "$MLOG"
  local RC=${PIPESTATUS[0]}
  if [ "$RC" -eq 124 ] || [ "$RC" -eq 137 ]; then
    echo "[driver] $name: TIMEOUT after 1200s wall-clock cap (workers likely died / SLURM job expired) RC=$RC"
  elif [ "$RC" -ne 0 ]; then
    echo "[driver] $name: manager exited nonzero RC=$RC"
  fi

  echo "[driver] $(date) $name: manager finished (RC=$RC). Cancelling worker job $JOBID..."
  scancel "$JOBID"
  sleep 5
  echo "[driver] $(date) ========== $name DONE. log=$MLOG =========="
}

MGR_ARGS="--scheduler taskvine --port $PORT --reference-dir /shared/minimap2-sv-data/reference --data-dir /shared/minimap2-sv-data/data --output-dir $LOCALCWD/output --cores-per-task 2 --windows 8"

run_condition "base-execution" /shared/vine-audit/mm2_workers_plain_small.sbatch \
  "source /shared/miniconda3/etc/profile.d/conda.sh && conda activate minimap2-sv && python -u minimap2_benchmark.py --name mm2-base-small $MGR_ARGS"

echo "[driver] $(date) SMOKE TEST DONE"
