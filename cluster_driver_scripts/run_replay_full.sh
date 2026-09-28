#!/bin/bash
# Fidelis Replay — full driver (worker + manager, both replayed inside their
# captured audit.sif containers). Combines replay_full.sbatch and
# replay_manager_only.sh into one end-to-end run, following the same
# run_condition() pattern as the other six conditions' drivers.
#
# REQUIRES both a manager-side and worker-side audit.sif from a prior Full
# Fidelis (async) run — see README "Fidelis Replay" for which existing
# captured runs have both (the original two-fixed-node deployment) vs.
# manager-only (the 8-node ablation runs in this repo).
#
# Usage:
#   ./run_replay_full.sh <worker_sbatch_file> <manager_audit.sif> <workflow_script.py> [workflow args...]

set -euo pipefail
export PATH=/opt/slurm/bin:$PATH

WORKER_SBATCH="${1:?Usage: $0 <worker_sbatch_file> <manager_audit.sif> <workflow_script.py> [args...]}"
MANAGER_SIF="${2:?}"
shift 2

if [ ! -f "$MANAGER_SIF" ]; then
    echo "ERROR: manager SIF not found: $MANAGER_SIF" >&2
    exit 1
fi

echo "[replay] $(date) submitting worker job from $WORKER_SBATCH"
JOBID=$(sbatch "$WORKER_SBATCH" | awk '{print $4}')
echo "[replay] submitted worker job $JOBID"

for i in $(seq 1 60); do
    STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
    echo "[replay] $(date) poll $i: state=${STATE:-GONE}"
    if [ "$STATE" = "RUNNING" ]; then
        echo "[replay] worker running, giving it time to connect..."
        sleep 30
        break
    fi
    if [ -z "$STATE" ]; then
        echo "[replay] ERROR: worker job disappeared before running" >&2
        exit 1
    fi
    sleep 10
done

echo "[replay] $(date) replaying manager inside $MANAGER_SIF ..."
apptainer exec --env TASKVINE_WARM_POOL=1 --env VINE_REPLAY_MODE=1 "$MANAGER_SIF" \
    python "$@"
RC=$?

echo "[replay] $(date) manager replay finished (RC=$RC). Cancelling worker job $JOBID..."
scancel "$JOBID"
sleep 5
echo "[replay] $(date) DONE"
