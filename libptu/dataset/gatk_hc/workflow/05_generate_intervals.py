#!/usr/bin/env python3
# 05_generate_intervals.py
#
# Scatter the reference into non-overlapping intervals for GATK HaplotypeCaller.
#
# In a real cohort pipeline, scatter intervals are produced by GATK's
# SplitIntervals tool which balances them by callable base count.  Here we
# produce a simple evenly-spaced scatter, but deliberately place some
# intervals over the STR regions embedded in the reference so that the
# corresponding GATK tasks will produce high-memory spikes — recreating the
# exact scenario where per-file eBPF attribution predicts peak RSS but
# /proc/PID/io totals cannot.
#
# Output files (in ./intervals/):
#   scatter_001.interval_list  ...  scatter_NNN.interval_list
#   intervals_manifest.tsv     — maps (scatter_id, chrom, start, end, has_str)
#                                 used by the run scripts and eBPF analysis
#
# Usage:  python3 05_generate_intervals.py

import argparse
import os
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

REF_FAI     = "reference/hg38_synthetic.fa.fai"
OUT_DIR     = "intervals"

# How many intervals to scatter per chromosome.
# Keep small (5 per chrom = 25 total) so the local test run is fast.
# Overridable via --intervals-per-chrom for scale experiments.
INTERVALS_PER_CHROM = 5

# STR positions embedded by 01_generate_reference.py are at random positions
# but with seed=42, the first STR on chr1 lands near position 312_000.
# We record approximate STR anchor positions per chrom so we can label
# intervals that overlap them in the manifest.
# (In practice you'd derive these from the reference or a BED file.)
APPROX_STR_POSITIONS = {
    "chr1": [312_000, 580_000, 910_000, 1_230_000, 1_560_000,
             1_700_000, 1_820_000, 1_950_000],
    "chr2": [290_000, 600_000, 950_000, 1_200_000, 1_500_000, 1_720_000],
    "chr3": [280_000, 530_000, 870_000, 1_100_000, 1_400_000],
    "chrX": [240_000, 500_000, 800_000, 1_050_000],
    "chrY": [120_000, 280_000],
}

PICARD_HEADER = """\
@HD\tVN:1.6
"""


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_fai(fai_path: str) -> list:
    """Return [(chrom, length), ...] in the order they appear in the FAI."""
    chroms = []
    with open(fai_path) as f:
        for line in f:
            parts = line.split("\t")
            chroms.append((parts[0], int(parts[1])))
    return chroms


def interval_has_str(chrom: str, start: int, end: int,
                     window: int = 50_000) -> bool:
    """True if any known STR anchor falls within [start-window, end+window]."""
    positions = APPROX_STR_POSITIONS.get(chrom, [])
    return any(start - window <= p <= end + window for p in positions)


def write_interval_list(path: str, chrom: str, start: int, end: int,
                         seq_dict_lines: list) -> None:
    """
    Write a Picard-format interval_list file.
    Format: @HD header + @SQ lines + one interval line.
    Coordinates are 1-based inclusive (Picard convention).
    """
    with open(path, "w") as f:
        for line in seq_dict_lines:
            f.write(line)
        # interval line: chrom  start  end  strand  name
        f.write(f"{chrom}\t{start}\t{end}\t+\t{chrom}:{start}-{end}\n")


def build_seq_dict_lines(chroms: list) -> list:
    """Build the @HD + @SQ header lines for the interval_list files."""
    lines = ["@HD\tVN:1.6\n"]
    for chrom, length in chroms:
        lines.append(f"@SQ\tSN:{chrom}\tLN:{length}\n")
    return lines


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Scatter reference into GATK intervals")
    parser.add_argument("--ref-fai", default=REF_FAI)
    parser.add_argument("--output-dir", default=OUT_DIR)
    parser.add_argument("--intervals-per-chrom", type=int, default=INTERVALS_PER_CHROM,
                         help="Scale knob: number of scatter intervals per chromosome "
                              "(default: %(default)s)")
    args = parser.parse_args()
    if args.intervals_per_chrom < 1:
        parser.error("--intervals-per-chrom must be positive")

    if not os.path.exists(args.ref_fai):
        print(f"ERROR: {args.ref_fai} not found. "
              "Run bash 02_index_reference.sh first.", file=sys.stderr)
        sys.exit(1)

    os.makedirs(args.output_dir, exist_ok=True)

    chroms = load_fai(args.ref_fai)
    seq_dict_lines = build_seq_dict_lines(chroms)

    manifest_rows = []   # (scatter_id, chrom, start, end, has_str, length)
    scatter_id = 0

    for chrom, chrom_len in chroms:
        chunk = chrom_len // args.intervals_per_chrom
        for i in range(args.intervals_per_chrom):
            start = i * chunk + 1          # 1-based
            end   = min((i + 1) * chunk, chrom_len)

            scatter_id += 1
            fname = f"scatter_{scatter_id:03d}.interval_list"
            fpath = os.path.join(args.output_dir, fname)

            write_interval_list(fpath, chrom, start, end, seq_dict_lines)

            has_str = interval_has_str(chrom, start, end)
            manifest_rows.append((
                scatter_id, fname, chrom, start, end,
                end - start + 1, has_str
            ))

    # Write manifest
    manifest_path = os.path.join(args.output_dir, "intervals_manifest.tsv")
    with open(manifest_path, "w") as f:
        f.write("scatter_id\tfilename\tchrom\tstart\tend\t"
                "length_bp\thas_str_region\n")
        for row in manifest_rows:
            f.write("\t".join(str(x) for x in row) + "\n")

    print(f"Wrote {scatter_id} interval_list files to ./{args.output_dir}/")
    print(f"Manifest: {manifest_path}")
    print()

    # Summary
    has_str_count = sum(1 for r in manifest_rows if r[6])
    print(f"  Intervals with STR regions: {has_str_count}/{scatter_id}")
    print(f"  These are the high-memory tasks in the Slurm array.")
    print()
    print("Interval breakdown:")
    print(f"  {'ID':>4}  {'File':<30}  {'Chrom':6}  "
          f"{'Start':>9}  {'End':>9}  {'STR':3}")
    print(f"  {'-'*4}  {'-'*30}  {'-'*6}  {'-'*9}  {'-'*9}  {'-'*3}")
    for sid, fname, chrom, start, end, length, has_str in manifest_rows:
        flag = "YES" if has_str else "   "
        print(f"  {sid:>4}  {fname:<30}  {chrom:6}  "
              f"{start:>9,}  {end:>9,}  {flag}")

    print()
    print("Next step: run  bash 06_run_local.sh  (single-task test)")
    print("      or:  sbatch 07_slurm_array_job.sh  (full Slurm array)")


if __name__ == "__main__":
    main()
