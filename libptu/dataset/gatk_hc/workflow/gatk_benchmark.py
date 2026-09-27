#!/usr/bin/env python3
"""
GATK HaplotypeCaller benchmark — persistent-manager TaskVine driver.

Adapted from nextflow/run_haplotypecaller_vine.py to match this project's
existing benchmark pattern (see mapreduce_benchmark.py,
image_convolution_benchmark.py, dv5_benchmark.py, rag_benchmark.py,
climate_trend_benchmark.py): ONE persistent TaskVine manager for the whole
run, with all (sample x scatter-interval) HaplotypeCaller tasks submitted
up front, and workers connecting externally (launched separately via
`vine_worker` / `ptu` / `libptu-launcher`, matching the sciunit-slurm
driver pattern in revised_experiments/ablation_study_plan.md).

Same rationale as minimap2_benchmark.py: the original nextflow driver
created a fresh vine.Manager(0) + `vine_worker --single-shot` per task,
which cannot be wrapped by the ablation study's audit conditions (those
need one long-lived, audited manager/worker pair across the entire run).

Only HaplotypeCaller runs through TaskVine here, matching the shipped
pipeline: alignment (bwa mem), sorting, and MarkDuplicates happen upstream
via 04_align_and_index.sh and are not part of this driver.

Usage
-----
  # 1. Prepare reference, reads, aligned+deduped BAMs, and scatter intervals
  #    (one-time, per scale) using the existing numbered scripts:
  python3 01_generate_reference.py
  bash 02_index_reference.sh
  python3 03_generate_reads.py
  bash 04_align_and_index.sh
  python3 05_generate_intervals.py --intervals-per-chrom 5

  # 2. Launch worker(s) separately (unaudited baseline example):
  vine_worker <manager-host> 9123 --cores 2

  # 3. Run the benchmark against the persistent manager:
  python3 gatk_benchmark.py --scheduler taskvine --port 9123

  # Enable warm-pool transparently via env var (no application change):
  TASKVINE_WARM_POOL=1 python3 gatk_benchmark.py --scheduler taskvine --port 9123
"""

import argparse
import json
import os
import shlex
import sys
import time
from pathlib import Path

MANAGER_NAME = "gatk-hc-manager"
MANAGER_PORTS = [9123, 9150]


# ─────────────────────────────────────────────────────────────────────────────
# Input discovery
# ─────────────────────────────────────────────────────────────────────────────
def discover_samples(bam_dir: Path):
    samples = []
    for bam in sorted(bam_dir.glob("*.bam")):
        bai = bam.with_suffix(".bam.bai")
        if not bai.is_file():
            bai = Path(str(bam) + ".bai")
        if bai.is_file():
            samples.append((bam.stem, bam, bai))
    if not samples:
        sys.exit(f"No BAM+BAI pairs found in {bam_dir}. Run 04_align_and_index.sh first.")
    return samples


def load_intervals_manifest(intervals_dir: Path):
    manifest_path = intervals_dir / "intervals_manifest.tsv"
    if not manifest_path.is_file():
        sys.exit(f"Manifest not found: {manifest_path}. Run 05_generate_intervals.py first.")
    intervals = []
    with manifest_path.open() as f:
        next(f)  # header
        for line in f:
            scatter_id, fname, chrom, start, end, length_bp, has_str = \
                line.rstrip("\n").split("\t")
            intervals.append({"id": scatter_id, "filename": fname})
    return intervals


# ─────────────────────────────────────────────────────────────────────────────
# Task construction (command logic lifted from nextflow/run_haplotypecaller_vine.py)
# ─────────────────────────────────────────────────────────────────────────────
def build_hc_task(manager, reference_file, sample_name, bam_file, bai_file,
                   interval_path: Path, out_dir: Path, cores: int, heap: str):
    import ndcctools.taskvine as vine
    output_name = f"{sample_name}_{interval_path.stem}.g.vcf.gz"

    command = shlex.join([
        "gatk", "--java-options", f"-Xmx{heap}", "HaplotypeCaller",
        "--reference", "reference/hg38_synthetic.fa",
        "--input", f"{sample_name}.bam",
        "--intervals", interval_path.name,
        "--output", output_name,
        "--emit-ref-confidence", "GVCF",
        "--native-pair-hmm-threads", str(cores),
        "--tmp-dir", ".", "--verbosity", "ERROR", "--QUIET", "true",
    ])

    task = vine.Task(command)
    task.set_cores(cores)
    task.add_input(reference_file, "reference")
    task.add_input(bam_file, f"{sample_name}.bam")
    task.add_input(bai_file, f"{sample_name}.bam.bai")
    task.add_input(manager.declare_file(str(interval_path.resolve())), interval_path.name)
    out_file = manager.declare_file(str((out_dir / output_name).resolve()))
    task.add_output(out_file, output_name)
    task.set_tag(f"{sample_name}_{interval_path.stem}")
    return task, output_name


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scheduler", choices=["threads", "taskvine"], default="taskvine")
    parser.add_argument("--reference-dir", default="reference",
                         help="Directory containing hg38_synthetic.fa[.fai/.dict]")
    parser.add_argument("--bam-dir", default="bam",
                         help="Directory containing aligned+deduped {sample}.bam/.bam.bai")
    parser.add_argument("--intervals-dir", default="intervals",
                         help="Directory containing scatter_*.interval_list + manifest")
    parser.add_argument("--output-dir", default="output")
    parser.add_argument("--samples", nargs="+", default=None,
                         help="Subset of sample names to run (default: all in --bam-dir)")
    parser.add_argument("--interval-count", type=int, default=None,
                         help="Number of scatter intervals to use (default: all in manifest)")
    parser.add_argument("--cores-per-task", type=int, default=1)
    parser.add_argument("--memory-mb", type=int, default=3072)
    parser.add_argument("--heap", default="2g")
    parser.add_argument("--name", default=os.environ.get("VINE_MANAGER_NAME", MANAGER_NAME))
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--ports", type=int, nargs="+",
                         default=[int(p) for p in
                                  os.environ.get("VINE_MANAGER_PORTS", "9123,9150").split(",")])
    args = parser.parse_args()

    reference_dir = Path(args.reference_dir)
    bam_dir = Path(args.bam_dir)
    intervals_dir = Path(args.intervals_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    reference_fa = reference_dir / "hg38_synthetic.fa"
    if not reference_fa.is_file():
        sys.exit(f"Reference not found: {reference_fa}. Run 01_generate_reference.py "
                  f"and 02_index_reference.sh first.")

    samples = discover_samples(bam_dir)
    if args.samples:
        samples = [s for s in samples if s[0] in args.samples]
        if not samples:
            sys.exit(f"None of --samples {args.samples} found in {bam_dir}")

    intervals = load_intervals_manifest(intervals_dir)
    if args.interval_count:
        intervals = intervals[:args.interval_count]

    total_tasks = len(samples) * len(intervals)
    print(f"Running HaplotypeCaller: {len(samples)} sample(s) x "
          f"{len(intervals)} interval(s) = {total_tasks} task(s)")

    start_time = time.time()

    if args.scheduler == "threads":
        import subprocess
        completed = 0
        for sample_name, bam, bai in samples:
            for iv in intervals:
                interval_path = intervals_dir / iv["filename"]
                out_path = output_dir / f"{sample_name}_{interval_path.stem}.g.vcf.gz"
                cmd = [
                    "gatk", "--java-options", f"-Xmx{args.heap}", "HaplotypeCaller",
                    "--reference", str(reference_fa),
                    "--input", str(bam),
                    "--intervals", str(interval_path),
                    "--output", str(out_path),
                    "--emit-ref-confidence", "GVCF",
                    "--native-pair-hmm-threads", str(args.cores_per_task),
                    "--tmp-dir", str(output_dir), "--verbosity", "ERROR", "--QUIET", "true",
                ]
                result = subprocess.run(cmd)
                if result.returncode == 0:
                    completed += 1
                else:
                    print(f"{sample_name}/{interval_path.stem} failed "
                          f"(exit {result.returncode})", file=sys.stderr)
        summary = {"tasks_total": total_tasks, "tasks_completed": completed}

    else:  # taskvine
        import ndcctools.taskvine as vine
        port = args.port if args.port else args.ports
        m = vine.Manager(port=port, name=args.name)
        print(f"TaskVine manager: name={args.name!r}, port(s)={port}")

        reference_file = m.declare_file(str(reference_dir.resolve()))
        pending = {}
        for sample_name, bam, bai in samples:
            bam_file = m.declare_file(str(bam.resolve()))
            bai_file = m.declare_file(str(bai.resolve()))
            for iv in intervals:
                interval_path = intervals_dir / iv["filename"]
                task, output_name = build_hc_task(
                    m, reference_file, sample_name, bam_file, bai_file,
                    interval_path, output_dir, args.cores_per_task, args.heap)
                task.set_memory(args.memory_mb)
                task_id = m.submit(task)
                pending[task_id] = (f"{sample_name}/{interval_path.stem}", output_name)

        print(f"Submitted {len(pending)} HaplotypeCaller tasks. Waiting for completion...")
        completed = 0
        failed = 0
        while not m.empty():
            t = m.wait(5)
            if t is None:
                continue
            label, output_name = pending.get(t.id, ("?", None))
            if t.successful() and output_name and (output_dir / output_name).is_file():
                completed += 1
            else:
                failed += 1
                print(f"{label} failed: result={t.result} exit_code={t.exit_code}",
                      file=sys.stderr)

        summary = {"tasks_total": total_tasks, "tasks_completed": completed,
                   "tasks_failed": failed}
        if hasattr(m, "workflow_summary"):
            m.workflow_summary()

    elapsed = time.time() - start_time
    summary["elapsed_seconds"] = elapsed
    print(f"Computation complete in {elapsed:.2f} seconds.")

    summary_path = output_dir / "run_summary.json"
    with summary_path.open("w") as f:
        json.dump(summary, f, indent=2)
    print(f"Summary written to: {summary_path}")

    if summary.get("tasks_failed", 0) or summary["tasks_completed"] < summary["tasks_total"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
