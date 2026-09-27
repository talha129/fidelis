#!/bin/bash
#SBATCH --job-name=mm2-libptu-sync
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:40:00
#SBATCH --output=/shared/vine-audit/mm2-libptu-sync-medium/workers_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate minimap2-sv
srun --ntasks=8 --ntasks-per-node=1 bash -c 'LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output /shared/vine-audit/mm2-libptu-sync-medium/worker_$(hostname) vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2'
