#!/bin/bash
#SBATCH --job-name=ctrend-libptu-sync
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=06:00:00
#SBATCH --output=/shared/vine-audit/ctrend-libptu-sync-small/workers_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate ctrend
srun --ntasks=8 --ntasks-per-node=1 bash -c 'LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output /shared/vine-audit/ctrend-libptu-sync-small/worker_$(hostname) vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2'
