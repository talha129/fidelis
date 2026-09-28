#!/bin/bash
# Fidelis Replay (manager-side) — replays the audited manager process inside
# its captured SIF container, using the same warm-pool mechanism as the
# original audit run, but with VINE_REPLAY_MODE=1 instead of VINE_AUDIT_MODE=1
# so no new capture happens.
#
# This script replays the MANAGER only. It assumes workers are started
# unaudited (plain `vine_worker`), which is sufficient to verify that a
# captured manager environment (Python interpreter, imported modules,
# workflow script and its Python-level dependencies) replays correctly.
#
# For full worker-side replay (replaying inside the workers' own captured
# containers), see replay_full.sh below and the README caveat about
# worker-side SIF availability.
#
# Usage:
#   ./replay_manager_only.sh <manager_audit.sif> <workflow_script.py> [workflow args...]
#
# Example (Minimap2, medium scale):
#   ./replay_manager_only.sh \
#       /shared/vine-audit/mm2-ablation-medium/manager-fidelis/audit.sif \
#       minimap2_benchmark.py --scheduler taskvine \
#       --reference-dir <ref-dir> --data-dir <data-dir> --cores-per-task 2 \
#       --ports 9123 9150

set -euo pipefail

SIF="${1:?Usage: $0 <manager_audit.sif> <workflow_script.py> [args...]}"
shift

if [ ! -f "$SIF" ]; then
    echo "ERROR: SIF not found: $SIF" >&2
    echo "This must be the audit.sif produced by a prior Full Fidelis (async)" >&2
    echo "run's manager-side materialize step, e.g.:" >&2
    echo "  /shared/vine-audit/<run>/manager-fidelis/audit.sif" >&2
    exit 1
fi

echo "[replay] Starting worker(s) unaudited — start these separately with:"
echo "  vine_worker <MANAGER_INTERNAL_IP> 9123 --cores <N>"
echo "[replay] Replaying manager inside $SIF ..."

apptainer exec --env TASKVINE_WARM_POOL=1 --env VINE_REPLAY_MODE=1 "$SIF" \
    python "$@"
