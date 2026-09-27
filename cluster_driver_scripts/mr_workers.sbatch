#!/bin/bash
#SBATCH --job-name=mr-base-medium3
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:40:00
#SBATCH --output=/shared/vine-audit/map-reduce-base-medium/workers_%j_%N.out

source /shared/miniconda3/etc/profile.d/conda.sh
conda activate map-reduce
srun --ntasks=8 --ntasks-per-node=1 vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 4
