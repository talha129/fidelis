#!/bin/bash
# Fidelis Replay (worker) — SLURM sbatch template.
#
# Replays a worker inside its own captured audit.sif, per
# ablation_study_plan.md §9.2's original two-fixed-node command pattern.
#
# REQUIRES a per-worker audit.sif captured during a prior Full Fidelis
# (async) run. This is only available for runs where the worker process
# was allowed to reach its own LIBPTU_BUILD_ON_EXIT materialize handler
# before exiting — i.e. NOT the cluster_driver_scripts/*_workers_fidelis*
# 8-node ablation runs in this repo, whose worker directories are
# intentionally cancelled (`scancel`) as soon as the manager finishes, before
# their on-exit materialize handler runs. See README "Fidelis Replay"
# section for which captured runs have real worker-side SIFs.
#
# Fill in SIF_PATH, MANAGER_HOST, and --feature below before submitting.

#SBATCH --job-name=fidelis-replay-worker
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:40:00

SIF_PATH="<PATH_TO_WORKER_AUDIT_SIF>"
DATA_BIND="<DATA_DIR>:<DATA_DIR>"   # bind-mount any input data the workflow reads at a fixed path

apptainer exec --bind "$DATA_BIND" "$SIF_PATH" \
    vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2 --feature "env-$(hostname)"
