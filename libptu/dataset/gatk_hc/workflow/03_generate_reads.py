#!/usr/bin/env python3
# 03_generate_reads.py
#
# Simulate paired-end Illumina reads from the synthetic reference for each
# sample.  Samples are given different mean coverage depths to mimic the
# real-world variation in BAM file sizes seen in a cohort study.
#
# Reads are written to ./fastq/sample_N_{1,2}.fastq.gz
#
# The coverage variation is important for the eBPF attribution experiment:
# high-coverage samples read more BAM bytes from the same genomic interval,
# leading to higher peak RSS in GATK's assembler — which is exactly the
# per-file feature that /proc/PID/io cannot distinguish.
#
# Usage:  python3 03_generate_reads.py [--sample sample_01] [--coverage 3]

import argparse
import gzip
import math
import os
import random
import sys

random.seed(99)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

REF_FA    = "reference/hg38_synthetic.fa"
FASTQ_DIR = "fastq"

READ_LENGTH = 150          # bp, standard Illumina short read
INSERT_MEAN = 380          # bp, mean insert size
INSERT_SD   = 50           # bp
ERROR_RATE  = 0.001        # per-base sequencing error rate
QUAL_CHAR   = "I"          # Phred 40 — high-quality synthetic reads

# Samples: (name, mean_coverage)
# Coverage varies to produce different BAM sizes and different peak RSS,
# which is the key feature the model must learn to predict.
SAMPLES = [
    ("sample_01", 10),    # low coverage  — ~2 GB BAM in real data
    ("sample_02", 25),    # medium        — ~5 GB BAM
    ("sample_03", 40),    # high          — ~8 GB BAM
    ("sample_04", 60),    # very high     — ~12 GB BAM
    ("sample_05", 15),    # low-medium
]

BASES = list("ACGT")
COMP  = str.maketrans("ACGT", "TGCA")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_reference(fa_path: str) -> dict:
    """Load FASTA into {chrom: sequence} dict."""
    chroms = {}
    name = None
    buf = []
    with open(fa_path) as f:
        for line in f:
            line = line.rstrip()
            if line.startswith(">"):
                if name is not None:
                    chroms[name] = "".join(buf)
                name = line[1:].split()[0]
                buf = []
            else:
                buf.append(line)
    if name is not None:
        chroms[name] = "".join(buf)
    total = sum(len(v) for v in chroms.values())
    print(f"  Loaded reference: {len(chroms)} chromosomes, "
          f"{total:,} bp total")
    return chroms


def revcomp(seq: str) -> str:
    return seq.translate(COMP)[::-1]


def mutate(seq: str, error_rate: float) -> str:
    if error_rate == 0:
        return seq
    seq = list(seq)
    for i in range(len(seq)):
        if random.random() < error_rate:
            seq[i] = random.choice([b for b in BASES if b != seq[i]])
    return "".join(seq)


def n_reads_for_coverage(genome_size: int, coverage: int,
                          read_length: int) -> int:
    """Number of read PAIRS needed for a given mean coverage depth."""
    return math.ceil(genome_size * coverage / (read_length * 2))


def sample_insert(chroms: dict) -> tuple:
    """
    Pick a random (chrom, start, insert_size) and return both mates.
    Returns (read1_seq, read2_seq, chrom, start) or None if out of bounds.
    """
    chrom = random.choice(list(chroms.keys()))
    seq = chroms[chrom]
    insert = max(READ_LENGTH * 2,
                 int(random.gauss(INSERT_MEAN, INSERT_SD)))
    if insert > len(seq) - 1:
        return None
    start = random.randint(0, len(seq) - insert)
    frag = seq[start: start + insert]

    # Replace any non-ACGT bases (shouldn't happen but just in case)
    frag = "".join(b if b in BASES else random.choice(BASES) for b in frag)

    r1 = mutate(frag[:READ_LENGTH], ERROR_RATE)
    r2 = mutate(revcomp(frag[-READ_LENGTH:]), ERROR_RATE)
    return r1, r2, chrom, start


def write_reads(sample: str, coverage: int, chroms: dict) -> None:
    os.makedirs(FASTQ_DIR, exist_ok=True)
    genome_size = sum(len(v) for v in chroms.values())
    n_pairs = n_reads_for_coverage(genome_size, coverage, READ_LENGTH)

    fq1 = os.path.join(FASTQ_DIR, f"{sample}_1.fastq.gz")
    fq2 = os.path.join(FASTQ_DIR, f"{sample}_2.fastq.gz")

    qual = QUAL_CHAR * READ_LENGTH

    print(f"  {sample}: coverage={coverage}x  pairs={n_pairs:,} → "
          f"{fq1}, {fq2}")

    with gzip.open(fq1, "wt") as f1, gzip.open(fq2, "wt") as f2:
        i = 0
        written = 0
        while written < n_pairs:
            result = sample_insert(chroms)
            if result is None:
                continue
            r1, r2, chrom, start = result
            name = f"@{sample}.{written + 1} {chrom}:{start}"
            f1.write(f"{name}/1\n{r1}\n+\n{qual}\n")
            f2.write(f"{name}/2\n{r2}\n+\n{qual}\n")
            written += 1


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Generate synthetic paired-end reads")
    parser.add_argument("--sample", choices=[name for name, _ in SAMPLES],
                        help="Generate one sample (default: all samples)")
    parser.add_argument("--coverage", type=int,
                        help="Override coverage for selected samples")
    args = parser.parse_args()
    if args.coverage is not None and args.coverage < 1:
        parser.error("--coverage must be at least 1")

    if not os.path.exists(REF_FA):
        print(f"ERROR: {REF_FA} not found. "
              "Run 01_generate_reference.py first.", file=sys.stderr)
        sys.exit(1)

    print(f"Loading reference from {REF_FA} ...")
    chroms = load_reference(REF_FA)

    selected_samples = [(name, args.coverage or coverage)
                        for name, coverage in SAMPLES
                        if args.sample is None or name == args.sample]
    print(f"\nGenerating reads for {len(selected_samples)} samples:")
    for sample, coverage in selected_samples:
        write_reads(sample, coverage, chroms)

    print(f"\nFASTQ files written to ./{FASTQ_DIR}/")
    print("Next step: run  bash 04_align_and_index.sh")


if __name__ == "__main__":
    main()
