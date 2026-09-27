#!/usr/bin/env bash
# 02_index_reference.sh
#
# Index the synthetic reference produced by 01_generate_reference.py.
#
# Produces:
#   reference/hg38_synthetic.fa.fai   — samtools FAI index
#   reference/hg38_synthetic.dict     — GATK/Picard sequence dictionary
#   reference/hg38_synthetic.fa.{amb,ann,bwt,pac,sa} — BWA index
#
# Run as:  bash 02_index_reference.sh
# Prerequisites: source ~/gatk_env.sh

set -euo pipefail

REF_DIR="reference"
REF_FA="${REF_DIR}/hg38_synthetic.fa"

if [ ! -f "${REF_FA}" ]; then
    echo "ERROR: ${REF_FA} not found. Run python3 01_generate_reference.py first."
    exit 1
fi

if [ -z "${GATK:-}" ]; then
    echo "ERROR: GATK not set. Run: source ~/gatk_env.sh"
    exit 1
fi

echo "=== [1/3] samtools faidx ==="
samtools faidx "${REF_FA}"
echo "  Wrote ${REF_FA}.fai"

echo ""
echo "=== [2/3] GATK CreateSequenceDictionary ==="
DICT="${REF_DIR}/hg38_synthetic.dict"
"${GATK}" CreateSequenceDictionary \
    --REFERENCE "${REF_FA}" \
    --OUTPUT "${DICT}" \
    --QUIET true
echo "  Wrote ${DICT}"

echo ""
echo "=== [3/3] BWA index ==="
bwa index "${REF_FA}" 2>&1 | grep -v "^\[bwa_index\].*0 seconds" || true
echo "  Wrote BWA index files"

echo ""
echo "Reference index files:"
ls -lh "${REF_DIR}/"
echo ""
echo "Next step: run  python3 03_generate_reads.py"
