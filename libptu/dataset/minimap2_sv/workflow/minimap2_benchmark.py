#!/usr/bin/env python3
"""
Minimap2 long-read alignment benchmark — persistent-manager TaskVine driver.

Adapted from nextflow/run_alignment_vine.py to match this project's existing
benchmark pattern (see mapreduce_benchmark.py, image_convolution_benchmark.py,
dv5_benchmark.py, rag_benchmark.py, climate_trend_benchmark.py): ONE
persistent TaskVine manager for the whole run, with all window-alignment
tasks submitted up front, and workers connecting externally (launched
separately via `vine_worker` / `ptu` / `libptu-launcher`, matching the
sciunit-slurm driver pattern in revised_experiments/ablation_study_plan.md).

Why this rewrite was needed
----------------------------
The original nextflow/run_alignment_vine.py created a brand-new
vine.Manager(0) (ephemeral port) plus a `vine_worker --single-shot` for
EVERY window, spawned and torn down inside a single Nextflow task process.
That works for a standalone Nextflow demo, but it cannot be plugged into
the ablation study's 6-condition matrix: conditions 2-6 depend on wrapping
one long-lived worker process (via `ptu`/`libptu-launcher`) and one
long-lived, audited manager process (via `TASKVINE_WARM_POOL`/
`VINE_AUDIT_MODE` env vars) across the ENTIRE workflow run, the same way
the other 5 workflows are driven. A fresh manager+worker pair per task
has no persistent process to attach an audit wrapper to, and defeats
context/warm-pool reuse entirely (a brand-new worker never has anything
"warm" to reuse).

Usage
-----
  # 1. Generate synthetic reference + reads + index (one-time, per scale):
  python3 02_generate_reference.py --output-dir reference
  python3 03_generate_long_reads.py --reference reference/genome.fa \\
      --annotations reference/genome_annotations.bed \\
      --output-dir data --windows 200
  minimap2 -x map-ont -t 4 -d reference/genome.mmi reference/genome.fa
  samtools faidx reference/genome.fa

  # 2. Launch worker(s) separately (unaudited baseline example):
  vine_worker <manager-host> 9123 --cores 2

  # 3. Run the benchmark against the persistent manager:
  python3 minimap2_benchmark.py --scheduler taskvine --port 9123 --windows 200

  # Enable warm-pool transparently via env var (no application change):
  TASKVINE_WARM_POOL=1 python3 minimap2_benchmark.py --scheduler taskvine --port 9123
"""

import argparse
import json
import os
import shlex
import sys
import time
from pathlib import Path

MANAGER_NAME = "minimap2-sv-manager"
MANAGER_PORTS = [9123, 9150]


# ─────────────────────────────────────────────────────────────────────────────
# Manifest / input discovery
# ─────────────────────────────────────────────────────────────────────────────
def load_manifest(data_dir: Path):
    manifest_path = data_dir / "window_manifest.tsv"
    if not manifest_path.is_file():
        sys.exit(f"Manifest not found: {manifest_path}. "
                  f"Run 03_generate_long_reads.py --output-dir {data_dir} first.")
    windows = []
    with manifest_path.open() as f:
        next(f)  # header
        for line in f:
            wid, fname, wtype, peak_gb, n_reads = line.rstrip("\n").split("\t")
            windows.append({"id": wid, "filename": fname, "type": wtype})
    return windows


# ─────────────────────────────────────────────────────────────────────────────
# Task construction (command logic lifted from nextflow/run_alignment_vine.py)
# ─────────────────────────────────────────────────────────────────────────────
def build_alignment_task(manager, index_file, reads_path: Path, out_dir: Path,
                          window_id: str, cores: int):
    import ndcctools.taskvine as vine
    bam_name = f"window_{window_id}.bam"
    bai_name = f"window_{window_id}.bam.bai"
    flagstat_name = f"window_{window_id}.flagstat.txt"

    minimap = shlex.join([
        "minimap2", "-a", "-x", "map-ont", "--MD", "-t", str(cores),
        "genome.mmi", reads_path.name,
    ])
    sort = shlex.join([
        "samtools", "sort", "-@", str(cores), "-m", "256M",
        "-o", bam_name, "-",
    ])
    index = shlex.join(["samtools", "index", bam_name, bai_name])
    flagstat = shlex.join(["samtools", "flagstat", bam_name])
    shell_command = (f"{minimap} | {sort} && {index} && "
                      f"{flagstat} > {shlex.quote(flagstat_name)}")
    command = shlex.join(["bash", "-o", "pipefail", "-c", shell_command])

    task = vine.Task(command)
    task.set_cores(cores)
    task.add_input(index_file, "genome.mmi")
    task.add_input(manager.declare_file(str(reads_path.resolve())), reads_path.name)
    out_bam = manager.declare_file(str((out_dir / bam_name).resolve()))
    out_bai = manager.declare_file(str((out_dir / bai_name).resolve()))
    out_flag = manager.declare_file(str((out_dir / flagstat_name).resolve()))
    task.add_output(out_bam, bam_name)
    task.add_output(out_bai, bai_name)
    task.add_output(out_flag, flagstat_name)
    task.set_tag(f"window_{window_id}")
    return task, (bam_name, bai_name, flagstat_name)


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--scheduler", choices=["threads", "taskvine"], default="taskvine")
    parser.add_argument("--reference-dir", default="reference",
                         help="Directory containing genome.fa / genome.mmi / genome.fa.fai")
    parser.add_argument("--data-dir", default="data",
                         help="Directory containing reads/ and window_manifest.tsv")
    parser.add_argument("--output-dir", default="output")
    parser.add_argument("--windows", type=int, default=None,
                         help="Number of windows to align (default: all in manifest)")
    parser.add_argument("--cores-per-task", type=int, default=1)
    parser.add_argument("--memory-mb", type=int, default=2048)
    parser.add_argument("--name", default=os.environ.get("VINE_MANAGER_NAME", MANAGER_NAME))
    parser.add_argument("--port", type=int, default=None)
    parser.add_argument("--ports", type=int, nargs="+",
                         default=[int(p) for p in
                                  os.environ.get("VINE_MANAGER_PORTS", "9123,9150").split(",")])
    args = parser.parse_args()

    reference_dir = Path(args.reference_dir)
    data_dir = Path(args.data_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    mmi_path = reference_dir / "genome.mmi"
    if not mmi_path.is_file():
        sys.exit(f"Index not found: {mmi_path}. Build it with:\n"
                  f"  minimap2 -x map-ont -d {mmi_path} {reference_dir / 'genome.fa'}")

    windows = load_manifest(data_dir)
    if args.windows:
        windows = windows[:args.windows]
    print(f"Aligning {len(windows)} windows from {data_dir}/reads/")

    start_time = time.time()

    if args.scheduler == "threads":
        # Local, unaudited sanity-check path: no TaskVine, just run the same
        # shell pipeline directly. Useful for validating wiring without a
        # manager/worker running.
        import subprocess
        completed = 0
        for w in windows:
            reads_path = data_dir / "reads" / w["filename"]
            bam = output_dir / f"window_{w['id']}.bam"
            bai = output_dir / f"window_{w['id']}.bam.bai"
            flagstat = output_dir / f"window_{w['id']}.flagstat.txt"
            cmd = (f"minimap2 -a -x map-ont --MD -t {args.cores_per_task} "
                   f"{shlex.quote(str(mmi_path))} {shlex.quote(str(reads_path))} | "
                   f"samtools sort -@ {args.cores_per_task} -m 256M -o {shlex.quote(str(bam))} - && "
                   f"samtools index {shlex.quote(str(bam))} {shlex.quote(str(bai))} && "
                   f"samtools flagstat {shlex.quote(str(bam))} > {shlex.quote(str(flagstat))}")
            result = subprocess.run(["bash", "-o", "pipefail", "-c", cmd])
            if result.returncode == 0:
                completed += 1
            else:
                print(f"window {w['id']} failed (exit {result.returncode})", file=sys.stderr)
        summary = {"windows_total": len(windows), "windows_completed": completed}

    else:  # taskvine
        import ndcctools.taskvine as vine
        port = args.port if args.port else args.ports
        m = vine.Manager(port=port, name=args.name)
        print(f"TaskVine manager: name={args.name!r}, port(s)={port}")

        index_file = m.declare_file(str(mmi_path.resolve()))
        pending = {}
        for w in windows:
            reads_path = data_dir / "reads" / w["filename"]
            task, outputs = build_alignment_task(
                m, index_file, reads_path, output_dir, w["id"], args.cores_per_task)
            task.set_memory(args.memory_mb)
            task_id = m.submit(task)
            pending[task_id] = (w["id"], outputs)

        print(f"Submitted {len(pending)} alignment tasks. Waiting for completion...")
        completed = 0
        failed = 0
        while not m.empty():
            t = m.wait(5)
            if t is None:
                continue
            window_id, outputs = pending.get(t.id, ("?", ()))
            if t.successful() and all((output_dir / name).is_file() for name in outputs):
                completed += 1
            else:
                failed += 1
                print(f"window {window_id} failed: result={t.result} exit_code={t.exit_code}",
                      file=sys.stderr)

        summary = {"windows_total": len(windows), "windows_completed": completed,
                   "windows_failed": failed}
        if hasattr(m, "workflow_summary"):
            m.workflow_summary()

    elapsed = time.time() - start_time
    summary["elapsed_seconds"] = elapsed
    print(f"Computation complete in {elapsed:.2f} seconds.")

    output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / "run_summary.json"
    with summary_path.open("w") as f:
        json.dump(summary, f, indent=2)
    print(f"Summary written to: {summary_path}")

    if summary.get("windows_failed", 0) or summary["windows_completed"] < summary["windows_total"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
