#!/bin/bash
#SBATCH --job-name=rag-plain
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:40:00
#SBATCH --output=/shared/vine-audit/rag-local-staging-medium/workers_plain_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate rag
srun --ntasks=8 --ntasks-per-node=1 vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2
