#!/usr/bin/env bash
# 04_align_and_index.sh
#
# For each sample:
#   1. Align paired-end FASTQ with bwa mem
#   2. Sort by coordinate with samtools sort
#   3. Mark duplicates with GATK MarkDuplicates
#   4. Index with samtools index
#
# Produces (in ./bam/):
#   sample_N.bam        coordinate-sorted, duplicate-marked BAM
#   sample_N.bam.bai    BAI index (htslib reads this via mmap at runtime)
#
# The BAI index is the implicit file the GATK job opens without listing it
# explicitly in the job script — it is mmap'd by htslib at startup to locate
# reads overlapping each scatter interval.
#
# Run as:  bash 04_align_and_index.sh
# Prerequisites: source ~/gatk_env.sh

set -euo pipefail

REF_FA="reference/hg38_synthetic.fa"
FASTQ_DIR="fastq"
BAM_DIR="bam"
THREADS=$(nproc)

if [ ! -f "${REF_FA}.bwt" ]; then
    echo "ERROR: BWA index not found. Run bash 02_index_reference.sh first."
    exit 1
fi

if [ -z "${GATK:-}" ]; then
    echo "ERROR: GATK not set. Run: source ~/gatk_env.sh"
    exit 1
fi

mkdir -p "${BAM_DIR}" tmp_sort

SAMPLES=(sample_01 sample_02 sample_03 sample_04 sample_05)

for SAMPLE in "${SAMPLES[@]}"; do
    FQ1="${FASTQ_DIR}/${SAMPLE}_1.fastq.gz"
    FQ2="${FASTQ_DIR}/${SAMPLE}_2.fastq.gz"
    BAM_SORTED="${BAM_DIR}/${SAMPLE}_sorted.bam"
    BAM_FINAL="${BAM_DIR}/${SAMPLE}.bam"
    METRICS="${BAM_DIR}/${SAMPLE}_dupmetrics.txt"

    if [ ! -f "${FQ1}" ]; then
        echo "ERROR: ${FQ1} not found. Run python3 03_generate_reads.py first."
        exit 1
    fi

    echo "=== Processing ${SAMPLE} ==="

    # --- Step 1: align -------------------------------------------------------
    echo "  [1/3] bwa mem (${THREADS} threads) ..."
    bwa mem \
        -t "${THREADS}" \
        -R "@RG\tID:${SAMPLE}\tSM:${SAMPLE}\tPL:ILLUMINA\tLB:lib1\tPU:unit1" \
        "${REF_FA}" "${FQ1}" "${FQ2}" 2>/dev/null \
      | samtools sort \
            -@ "${THREADS}" \
            -T "tmp_sort/${SAMPLE}" \
            -o "${BAM_SORTED}"

    # --- Step 2: mark duplicates (GATK) -------------------------------------
    echo "  [2/3] MarkDuplicates ..."
    "${GATK}" MarkDuplicates \
        --INPUT "${BAM_SORTED}" \
        --OUTPUT "${BAM_FINAL}" \
        --METRICS_FILE "${METRICS}" \
        --TMP_DIR tmp_sort \
        --QUIET true \
        --VERBOSITY ERROR 2>/dev/null
    rm -f "${BAM_SORTED}"

    # --- Step 3: index -------------------------------------------------------
    echo "  [3/3] samtools index ..."
    samtools index -@ "${THREADS}" "${BAM_FINAL}"

    BAM_SIZE=$(du -sh "${BAM_FINAL}" | cut -f1)
    BAI_SIZE=$(du -sh "${BAM_FINAL}.bai" | cut -f1)
    echo "  Done: ${BAM_FINAL} (${BAM_SIZE})  index (${BAI_SIZE})"
    echo ""
done

rm -rf tmp_sort

echo "All BAM files:"
ls -lh "${BAM_DIR}/"*.bam "${BAM_DIR}/"*.bai 2>/dev/null
echo ""
echo "Next step: run  python3 05_generate_intervals.py"
