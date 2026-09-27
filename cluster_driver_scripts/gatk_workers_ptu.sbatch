#!/bin/bash
#SBATCH --job-name=gatk-ptu
#SBATCH --nodes=8
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:40:00
#SBATCH --chdir=/tmp
#SBATCH --output=/shared/vine-audit/gatk-ablation-medium/workers_ptu_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate gatk-hc
srun --ntasks=8 --ntasks-per-node=1 /shared/provenance-to-use/ptu vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2
