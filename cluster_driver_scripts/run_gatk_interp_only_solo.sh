#!/bin/bash
export PATH=/opt/slurm/bin:$PATH
BASE=/shared/vine-audit/gatk-libptu-sync-medium
WFDIR=/shared/libptu/dataset/gatk_hc/workflow
LOCALCWD=/tmp/gatk-sync-local-test2
PORT=9123
mkdir -p "$BASE" "$LOCALCWD"
rm -rf "$LOCALCWD"/vine-run-info "$LOCALCWD"/__pycache__ "$LOCALCWD"/output

JOBID=$(sbatch /shared/vine-audit/gatk_workers_libptu_sync.sbatch | awk '{print $4}')
echo "submitted $JOBID"
for i in $(seq 1 60); do
  STATE=$(squeue -j "$JOBID" -h -o '%T' 2>/dev/null | head -1)
  if [ "$STATE" = "RUNNING" ]; then break; fi
  sleep 10
done
sleep 30
DATA=/shared/gatk-hc-data-medium
cd "$LOCALCWD" && source /shared/miniconda3/etc/profile.d/conda.sh && conda activate gatk-hc &&   VINE_AUDIT_MODE=1 LIBPTU_BUILD_ON_EXIT=1 LIBPTU_KEEP_CDEROOT=1 /shared/libptu-sync/libptu-launcher --output $BASE/manager-interp-only-v2   python -u $WFDIR/gatk_benchmark.py --scheduler taskvine --name gatk-interp-only-v2 --reference-dir $DATA/reference --bam-dir $DATA/bam --intervals-dir $DATA/intervals --output-dir output --cores-per-task 2 --heap 2g --ports $PORT 9150
scancel "$JOBID"
echo '--- perf log ---'
tail -1 "$LOCALCWD"/vine-run-info/2*/vine-logs/performance
echo DONE_MARKER
