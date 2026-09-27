#!/bin/bash
#SBATCH --job-name=mm2-plain-c2
#SBATCH --nodes=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --partition=ubuntu22
#SBATCH --time=00:30:00
#SBATCH --output=/shared/vine-audit/minimap2-sv-small/workers_plain_%j_%N.out
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate minimap2-sv
srun --ntasks=2 --ntasks-per-node=1 vine_worker <MANAGER_INTERNAL_IP> 9123 --cores 2
