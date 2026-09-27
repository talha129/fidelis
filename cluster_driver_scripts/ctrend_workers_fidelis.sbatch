#!/bin/bash
#SBATCH --job-name=ctrend-fidelis
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:40:00
#SBATCH --output=/shared/vine-audit/ctrend-ablation-medium/workers_fidelis_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate ctrend
srun --ntasks=8 --ntasks-per-node=1 bash -c 'LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output /shared/vine-audit/ctrend-ablation-medium/worker_$(hostname) vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2'
