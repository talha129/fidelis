#!/usr/bin/env python3
# minimap2_sv/03_generate_long_reads.py
#
# Simulate ONT long reads for three window types.
# Window type determines minimap2 memory behaviour:
#
#   unique_window  — reads from gene-dense regions
#                    .mmi mmap bytes ≈ 7GB, heap ≈ 100MB/thread → peak ~8GB
#   segdup_window  — reads spanning segmental duplication junctions
#                    .mmi mmap bytes ≈ 7GB, heap ≈ 1.5GB/thread → peak ~12GB
#   tr_window      — ultra-long reads spanning tandem repeat expansions
#                    .mmi mmap bytes ≈ 7GB, heap ≈ 5–15GB/thread → peak ~30GB+
#
# IMPORTANT: The .mmi mmap bytes are nearly identical for all types.
# /proc/PID/io and eBPF file attribution CANNOT distinguish them.
# This is the hard negative case — reference annotation is required.
#
# Output: /opt/minimap2_sv/data/reads/window_NNN.fastq.gz
#         /opt/minimap2_sv/data/window_manifest.tsv

import argparse
import gzip, os, sys, random
import numpy as np

random.seed(7777); np.random.seed(7777)

GENOME_FA  = "/opt/minimap2_sv/reference/genome.fa"
ANNOT_BED  = "/opt/minimap2_sv/reference/genome_annotations.bed"
OUT_DIR    = "/opt/minimap2_sv/data/reads"
N_WINDOWS  = 200
BASES      = list("ACGT")
COMP       = str.maketrans("ACGT","TGCA")

READ_PARAMS = {
    "unique_window": {"mean":15000,"sd":8000,"min":5000,"max":40000,"n":3000},
    "segdup_window": {"mean":25000,"sd":12000,"min":8000,"max":60000,"n":2000},
    "tr_window":     {"mean":80000,"sd":40000,"min":20000,"max":200000,"n":800},
}
WINDOW_TYPES = {"unique_window":0.40,"segdup_window":0.35,"tr_window":0.25}
PEAK_GB      = {"unique_window":"8","segdup_window":"12","tr_window":"30+"}


def load_genome(fa):
    g = {}; name = None; buf = []
    for line in open(fa):
        line = line.rstrip()
        if line.startswith(">"):
            if name: g[name]="".join(buf)
            name, buf = line[1:].split()[0], []
        else: buf.append(line)
    if name: g[name]="".join(buf)
    return g


def load_anns(bed):
    a = {"segdup":[],"TR_expanded":[]}
    for line in open(bed):
        if line.startswith("chrom"): continue
        p = line.strip().split("\t")
        for k in a:
            if p[3].startswith(k):
                a[k].append((p[0],int(p[1]),int(p[2])))
    return a


def ont_error(seq, rate=0.08):
    out = []; i = 0
    while i < len(seq):
        r = random.random()
        if   r < rate/3:   out.append(random.choice(BASES)); i+=1
        elif r < 2*rate/3: out.append(random.choice(BASES))
        elif r < rate:     i+=1
        else:              out.append(seq[i]); i+=1
    return "".join(out)


def sample_read(genome, chrom, rs, re, tlen):
    seq = genome.get(chrom,"")
    actual = min(tlen, re - rs)
    if actual < 1000 or re - rs < 1: return None
    fs = max(0, min(random.randint(rs, max(rs, re-actual)), len(seq)-actual-1))
    frag = seq[fs:fs+actual]
    frag = "".join(b if b in set(BASES) else random.choice(BASES) for b in frag)
    if random.random() < 0.5:
        frag = frag[::-1].translate(COMP)
    return ont_error(frag)


def write_window(wid, wtype, genome, anns, out_dir, reads_per_window=None):
    p = READ_PARAMS[wtype]
    path = os.path.join(out_dir, f"window_{wid:03d}.fastq.gz")

    if wtype == "unique_window":
        chrom = random.choice(list(genome.keys()))
        regs  = [(chrom, random.randint(1000, len(genome[chrom])//2),
                  random.randint(len(genome[chrom])//2, len(genome[chrom])-1000))]
    elif wtype == "segdup_window":
        regs = anns.get("segdup",[])
        if not regs:
            chrom = list(genome.keys())[0]
            regs = [(chrom, 10000, 100000)]
    else:
        regs = anns.get("TR_expanded",[])
        if not regs:
            chrom = list(genome.keys())[0]
            regs = [(chrom, 10000, 500000)]

    written = 0
    with gzip.open(path, "wt") as fq:
        for _ in range(reads_per_window or p["n"]):
            rl = int(np.random.normal(p["mean"], p["sd"]))
            rl = int(np.clip(rl, p["min"], p["max"]))
            reg = random.choice(regs)
            chrom, rs, re = reg
            if wtype == "tr_window":
                rs = max(0, rs-5000)
                re = min(len(genome.get(chrom,"x"))-1, re+5000)
            rd = sample_read(genome, chrom, rs, re, rl)
            if not rd or len(rd) < 1000: continue
            qual = "0" * len(rd)
            fq.write(f"@w{wid}_r{written+1} type={wtype} len={len(rd)}\n"
                     f"{rd}\n+\n{qual}\n")
            written += 1
    return written


def main():
    parser = argparse.ArgumentParser(description="Generate synthetic ONT read windows")
    parser.add_argument("--reference", default=GENOME_FA)
    parser.add_argument("--annotations", default=ANNOT_BED)
    parser.add_argument("--output-dir", default=os.path.dirname(OUT_DIR),
                        help="Directory containing reads/ and window_manifest.tsv")
    parser.add_argument("--windows", type=int, default=N_WINDOWS)
    parser.add_argument("--window-type", choices=READ_PARAMS,
                        help="Use one window type instead of the default mixture")
    parser.add_argument("--reads-per-window", type=int,
                        help="Override read count for every window")
    args = parser.parse_args()
    if args.windows < 1 or (args.reads_per_window is not None and args.reads_per_window < 1):
        parser.error("--windows and --reads-per-window must be positive")

    if not os.path.exists(args.reference):
        sys.exit("Run 02_generate_reference.py first.")
    genome = load_genome(args.reference)
    anns   = load_anns(args.annotations) if os.path.exists(args.annotations) else {}

    reads_dir = os.path.join(args.output_dir, "reads")
    os.makedirs(reads_dir, exist_ok=True)
    types = []
    if args.window_type:
        types = [args.window_type] * args.windows
    else:
        for t, f in WINDOW_TYPES.items():
            types += [t]*int(args.windows*f)
        while len(types) < args.windows: types.append("tr_window")
        random.shuffle(types); types = types[:args.windows]

    manifest = []
    print(f"Generating {args.windows} windows:")
    for i, wtype in enumerate(types):
        wid = i+1
        n = write_window(wid, wtype, genome, anns, reads_dir, args.reads_per_window)
        expected_peak = PEAK_GB[wtype] if args.reads_per_window is None else "NA"
        manifest.append((wid, f"window_{wid:03d}.fastq.gz",
                         wtype, expected_peak, n))
        if wid % 50 == 0 or wid == args.windows:
            print(f"  [{wid:3d}/{args.windows}] {wtype:18s} reads={n}")

    mpath = os.path.join(args.output_dir, "window_manifest.tsv")
    with open(mpath, "w") as f:
        f.write("window_id\tfilename\twindow_type\texpected_peak_gb\tn_reads\n")
        for row in manifest:
            f.write("\t".join(str(x) for x in row)+"\n")

    print(f"\nManifest: {mpath}")
    print("Next: bash 04_build_index.sh")


if __name__ == "__main__":
    main()
