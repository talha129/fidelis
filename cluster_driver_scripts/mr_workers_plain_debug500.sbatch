#!/bin/bash
#SBATCH --job-name=mr-warmpool-debug500
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:20:00
#SBATCH --output=/shared/vine-audit/debug-warmpool-500/workers_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate map-reduce
srun --ntasks=8 --ntasks-per-node=1 vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2
