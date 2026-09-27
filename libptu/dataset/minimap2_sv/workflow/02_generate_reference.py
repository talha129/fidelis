#!/usr/bin/env python3
# minimap2_sv/02_generate_reference.py
#
# Generate a synthetic reference with three complexity classes:
#   unique_sequence       → minimizer hits ≈ 1–5   → peak ~8 GB
#   segdup_regions        → minimizer hits ≈ 100–500 → peak ~12 GB
#   tandem_repeat_regions → minimizer hits ≈ 10^4–10^6 → peak ~30+ GB
#
# The tandem repeat expansions are disease-associated repeats:
# C9orf72 (ALS), HTT (Huntington), DMPK (DM1), FXN (Friedreich ataxia).
# Reads spanning these cause minimap2's seed hit list to explode.
#
# Output: /opt/minimap2_sv/reference/genome.fa
#         /opt/minimap2_sv/reference/genome_annotations.bed

import argparse
import os, random
import numpy as np

random.seed(1234); np.random.seed(1234)

OUT_FA  = "/opt/minimap2_sv/reference/genome.fa"
OUT_BED = "/opt/minimap2_sv/reference/genome_annotations.bed"
LINE    = 60
BASES   = list("ACGT")

TR_EXPANSIONS = [
    ("GGGGCC", 800, 10,  "C9orf72_ALS"),
    ("CAG",    200, 20,  "HTT_Huntington"),
    ("CTG",    800, 15,  "DMPK_DM1"),
    ("GAA",    700,  8,  "FXN_Friedreich"),
    ("CGG",    200, 30,  "FMR1_FragileX"),
    ("AAGGG",  400,  5,  "RFC1_CANVAS"),
    ("ATTCT",  600, 10,  "ATXN10_SCA10"),
    ("TGGAA",  400,  8,  "ATXN8_SCA8"),
]

CHROMS = [
    ("chr1", 5_000_000, 4, 3),
    ("chr2", 4_000_000, 3, 2),
    ("chr3", 3_000_000, 3, 2),
    ("chrX", 3_000_000, 3, 1),
    ("chrY", 1_000_000, 2, 2),
]

SMALL_CHROMS = [("chr1", 500_000, 1, 1)]


def rseq(n, gc=0.42):
    out = []
    for _ in range(n):
        r = random.random()
        if r < gc/2:          out.append("G")
        elif r < gc:          out.append("C")
        elif r < gc+(1-gc)/2: out.append("A")
        else:                 out.append("T")
    return "".join(out)


def mutate(s, rate):
    return "".join(
        random.choice([b for b in BASES if b != c])
        if random.random() < rate else c
        for c in s
    )


def build_chrom(name, length, n_segdups, n_tr):
    seq = list(rseq(length))
    anns = []
    used = []

    def place(content, label):
        cl = len(content)
        if cl >= length - 1000:
            return False
        for _ in range(500):
            s = random.randint(1000, length - cl - 1000)
            if all(abs(s - u[0]) > cl + 500 for u in used):
                for i, c in enumerate(content):
                    seq[s + i] = c
                used.append((s, s + cl))
                anns.append((name, s, s + cl, label))
                return True
        return False

    # Segmental duplications: primary + near-identical copies
    for sdx in range(n_segdups):
        plen   = random.randint(8000, 15000)
        pseq   = rseq(plen, gc=0.48)
        n_cop  = random.randint(3, 6)
        place(pseq, f"segdup_{sdx}_copy0")
        for c in range(1, n_cop):
            place(mutate(pseq, 0.03), f"segdup_{sdx}_copy{c}")

    # Tandem repeat expansions
    chosen = random.sample(TR_EXPANSIONS, min(n_tr, len(TR_EXPANSIONS)))
    for motif, n_exp, n_norm, disease in chosen:
        exp_seq  = rseq(500) + motif * n_exp  + rseq(500)
        norm_seq = rseq(500) + motif * n_norm + rseq(500)
        place(exp_seq,  f"TR_expanded_{disease}")
        place(norm_seq, f"TR_normal_{disease}")

    return "".join(seq), anns


def main():
    parser = argparse.ArgumentParser(description="Generate a synthetic minimap2 reference")
    parser.add_argument("--output-dir", default=os.path.dirname(OUT_FA),
                        help="Directory for genome.fa and genome_annotations.bed")
    parser.add_argument("--small", action="store_true",
                        help="Generate one 500 kb chromosome for a small workflow test")
    args = parser.parse_args()
    out_fa = os.path.join(args.output_dir, "genome.fa")
    out_bed = os.path.join(args.output_dir, "genome_annotations.bed")
    chroms = SMALL_CHROMS if args.small else CHROMS

    os.makedirs(args.output_dir, exist_ok=True)
    all_anns = []
    print("Generating synthetic minimap2 SV reference ...")
    with open(out_fa, "w") as fa:
        for name, length, n_s, n_t in chroms:
            print(f"  {name}: {length:,}bp  segdups={n_s}  TR_expansions={n_t}")
            seq, anns = build_chrom(name, length, n_s, n_t)
            fa.write(f">{name}\n")
            for i in range(0, len(seq), LINE):
                fa.write(seq[i:i+LINE]+"\n")
            all_anns.extend(anns)

    with open(out_bed, "w") as bed:
        bed.write("chrom\tstart\tend\tannotation\n")
        for row in sorted(all_anns):
            bed.write("\t".join(str(x) for x in row)+"\n")

    n_tr  = sum(1 for _,_,_,a in all_anns if a.startswith("TR_expanded"))
    n_sd  = sum(1 for _,_,_,a in all_anns if a.startswith("segdup"))
    size  = os.path.getsize(out_fa)/1e6
    print(f"\nReference: {out_fa} ({size:.1f}MB)")
    print(f"TR_expanded={n_tr}  segdup_copies={n_sd}")
    print("Next: python3 03_generate_long_reads.py")


if __name__ == "__main__":
    main()
