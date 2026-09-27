#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/dconv-local-staging-medium
IMG=/shared/libptu/dataset/distributed_image_convolution/workflow/npp.jpg

echo "[debug-driver] $(date +%s.%N) Submitting worker job..."
JOBID=$(sbatch /shared/vine-audit/dconv_workers_fidelis_v2.sbatch | awk '{print $4}')
echo "[debug-driver] Submitted job $JOBID"

for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o "%T" 2>/dev/null | head -1)
  echo "[debug-driver] $(date +%s.%N) poll $i: state=${STATE:-GONE}"
  if [ "$STATE" = "RUNNING" ]; then
    echo "[debug-driver] job RUNNING -- giving 25s connect buffer"
    sleep 25
    break
  fi
  if [ -z "$STATE" ]; then echo "[debug-driver] ERROR job disappeared"; exit 1; fi
  sleep 10
done

echo "[debug-driver] $(date +%s.%N) MANAGER_LAUNCH_START (CWD=local /tmp)"
cd /tmp/dconv-local-test
mkdir -p output
source /shared/miniconda3/etc/profile.d/conda.sh
conda activate dconv
TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 \
  /shared/libptu/libptu-launcher --output $BASE/manager \
  python -u image_convolution_benchmark.py --name dconv-localstage --ports 9123 9150 \
  --images $IMG $IMG $IMG $IMG $IMG --kernels sharpen --tile-size 256 --output-dir /tmp/dconv-local-test/output \
  2>&1 | tee $BASE/manager_debug.log

echo "[debug-driver] $(date +%s.%N) MANAGER_LAUNCH_END"
scancel "$JOBID"
echo "[debug-driver] $(date +%s.%N) DONE"
