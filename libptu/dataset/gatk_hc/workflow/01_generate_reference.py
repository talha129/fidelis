#!/usr/bin/env python3
# 01_generate_reference.py
#
# Generate a synthetic human-like reference FASTA with realistic base
# composition, GC variation between chromosomes, and embedded low-complexity
# (STR) regions that will stress GATK's haplotype assembler — mirroring the
# memory spike behaviour described in the design doc.
#
# Output files (written to ./reference/):
#   hg38_synthetic.fa        FASTA reference
#   hg38_synthetic.fa.fai    samtools fai index (written by caller script)
#   hg38_synthetic.dict      GATK sequence dictionary (written by caller script)
#
# Usage:  python3 01_generate_reference.py [--small]

import argparse
import random
import os
import sys

random.seed(42)

# ---------------------------------------------------------------------------
# Configuration — keep chromosomes small so BWA indexing and alignment
# complete quickly on a single machine.  Real hg38 chromosomes are ~100–250 Mb;
# we use 2 Mb which is enough for realistic interval scatter and BAM coverage.
# ---------------------------------------------------------------------------

CHROMOSOMES = [
    # name,  length,   gc_frac,  n_str_regions
    ("chr1",  2_000_000, 0.41,   8),   # near-average GC
    ("chr2",  1_800_000, 0.40,   6),
    ("chr3",  1_600_000, 0.39,   5),
    ("chrX",  1_200_000, 0.39,   4),
    ("chrY",    400_000, 0.37,   2),   # small, low coverage
]

STR_MOTIFS = [
    "AC", "AT", "AG", "CA", "CT", "GA", "GT", "TA",   # dinucleotide
    "AAT", "AAC", "ATG", "AGC",                         # trinucleotide
    "AAAT", "AAAC", "AAAG", "AATG",                     # tetranucleotide
]

OUT_DIR = "reference"
REF_NAME = "hg38_synthetic"
LINE_WIDTH = 60


def random_base(gc_frac: float) -> str:
    r = random.random()
    if r < gc_frac / 2:
        return "G"
    elif r < gc_frac:
        return "C"
    elif r < gc_frac + (1 - gc_frac) / 2:
        return "A"
    else:
        return "T"


def make_str_region(motif: str, copies: int) -> str:
    return motif * copies


def generate_chromosome(name: str, length: int, gc_frac: float,
                         n_str: int) -> str:
    """
    Generate a chromosome sequence with:
      - Random background bases at the given GC fraction
      - n_str embedded STR (short tandem repeat) regions
        These produce the low-complexity intervals that cause GATK's
        haplotype graph to grow large and spike memory.
    """
    seq = bytearray()
    for _ in range(length):
        seq.append(ord(random_base(gc_frac)))

    # Embed STR regions at random non-overlapping positions
    str_positions = []
    for _ in range(n_str):
        motif = random.choice(STR_MOTIFS)
        copies = random.randint(20, 80)          # 40–160 bp STR
        str_seq = make_str_region(motif, copies)
        max_start = length - len(str_seq) - 1
        if max_start < 0:
            continue
        start = random.randint(1000, max_start)
        # Avoid overlapping previous STRs
        overlap = any(abs(start - p) < 200 for p in str_positions)
        if not overlap:
            for i, b in enumerate(str_seq.encode()):
                seq[start + i] = b
            str_positions.append(start)

    return seq.decode()


def write_fasta(path: str, chromosomes: list) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        for name, seq in chromosomes:
            f.write(f">{name}\n")
            for i in range(0, len(seq), LINE_WIDTH):
                f.write(seq[i:i + LINE_WIDTH] + "\n")
    print(f"  Wrote {path}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate a synthetic reference")
    parser.add_argument("--small", action="store_true",
                        help="Generate a 100 kb chr1 smoke-test reference without STRs")
    args = parser.parse_args()

    os.makedirs(OUT_DIR, exist_ok=True)
    fasta_path = os.path.join(OUT_DIR, f"{REF_NAME}.fa")

    print("Generating synthetic chromosomes:")
    chrom_seqs = []
    chromosomes = [("chr1", 100_000, 0.41, 0)] if args.small else CHROMOSOMES
    for name, length, gc, n_str in chromosomes:
        print(f"  {name:6s}  length={length:>9,}  gc={gc:.0%}  "
              f"str_regions={n_str}")
        seq = generate_chromosome(name, length, gc, n_str)
        chrom_seqs.append((name, seq))

    print(f"\nWriting FASTA → {fasta_path}")
    write_fasta(fasta_path, chrom_seqs)

    total_bp = sum(len(s) for _, s in chrom_seqs)
    print(f"\nTotal reference size: {total_bp:,} bp  "
          f"({total_bp / 1e6:.1f} Mb)")
    print("\nNext step: run  bash 02_index_reference.sh")


if __name__ == "__main__":
    main()
