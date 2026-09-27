#!/bin/bash
#SBATCH --job-name=dconv-plain
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=06:00:00
#SBATCH --output=/shared/vine-audit/dconv-ablation-large/workers_plain_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate dconv
srun --ntasks=8 --ntasks-per-node=1 vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2
