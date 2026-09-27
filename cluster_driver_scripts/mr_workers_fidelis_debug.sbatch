#!/bin/bash
#SBATCH --job-name=mr-fidelis-debug
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:20:00
#SBATCH --output=/shared/vine-audit/debug-fidelis-trivial/workers_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate map-reduce
srun --ntasks=8 --ntasks-per-node=1 bash -c 'echo "[worker $(hostname)] launcher starting at $(date +%s.%N)"; LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu/libptu-launcher --output /shared/vine-audit/debug-fidelis-trivial/worker_$(hostname) vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2'
