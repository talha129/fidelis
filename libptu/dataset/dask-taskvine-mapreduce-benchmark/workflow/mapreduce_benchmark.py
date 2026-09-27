#!/usr/bin/env python3
"""
Map-Combine-Reduce benchmark.

Usage
-----
  python mapreduce_benchmark.py --generate
  python mapreduce_benchmark.py --scheduler threads
  python mapreduce_benchmark.py --scheduler taskvine  --port 9123 --name my-manager

  # Enable warm-pool transparently via env var (no application change):
  TASKVINE_WARM_POOL=1 python mapreduce_benchmark.py --scheduler taskvine --port 9123
"""

import argparse
import csv
import json
import os
import time
from collections import defaultdict
from math import inf
from pathlib import Path
from typing import Dict, List

import dask
from dask import delayed

# ─────────────────────────────────────────────────────────────────────────────
# Global configuration
# ─────────────────────────────────────────────────────────────────────────────

RECORDS_PER_FILE = 5_000
GROUPS           = ["A", "B", "C", "D"]
RNG_SEED         = 42

GROUP_MEANS = {"A": 10.0, "B": 20.0, "C": 30.0, "D": 40.0}
GROUP_STDS  = {"A": 2.0,  "B": 4.0,  "C": 6.0,  "D": 8.0}

INPUT_DIR   = "/shared/map-reduce-data"
OUTPUT_DIR  = "results"
OUTPUT_JSON = f"{OUTPUT_DIR}/final_summary.json"

MANAGER_NAME  = "dask-taskvine-mapreduce-manager"
MANAGER_PORT  = 9123


# ─────────────────────────────────────────────────────────────────────────────
# Workflow functions
# ─────────────────────────────────────────────────────────────────────────────

@delayed
def map_file_to_stats(input_path: str) -> Dict:
    """
    MAP STEP:
      - Read one input file (JSONL / JSON / CSV)
      - Computes per-file statistics
      - Returns a dict (TaskVine manages the data transfer)
    """
    p = Path(input_path)

    count = 0
    total = 0.0
    min_value = inf
    max_value = -inf
    group_counts = defaultdict(int)

    records = []
    try:
        with p.open("r") as f:
            first = f.readline()
            if not first.strip():
                records = []
            else:
                if first.lstrip().startswith("{"):
                    f.seek(0)
                    records = [json.loads(line) for line in f if line.strip()]
                else:
                    f.seek(0)
                    data = json.load(f)
                    records = data if isinstance(data, list) else [data]
    except Exception:
        with p.open("r", newline="") as f:
            reader = csv.DictReader(f)
            records = list(reader)

    for rec in records:
        try:
            value = float(rec.get("value", 0.0))
        except (TypeError, ValueError):
            continue

        group = rec.get("group", "unknown")
        group_counts[group] += 1

        count += 1
        total += value

        if value < min_value:
            min_value = value
        if value > max_value:
            max_value = value

    if count == 0:
        min_out = None
        max_out = None
        mean_value = None
    else:
        min_out = float(min_value)
        max_out = float(max_value)
        mean_value = total / count

    return {
        "file": str(p),
        "count": int(count),
        "sum_value": float(total),
        "mean_value": mean_value,
        "min_value": min_out,
        "max_value": max_out,
        "group_counts": dict(group_counts),
    }


@delayed
def combine_stats_files(stats_list: List[Dict]) -> Dict:
    """
    COMBINE STEP:
      - Takes a list of stats dicts
      - Aggregates counts/sums/min/max/group_counts
    """
    total_count = 0
    total_sum = 0.0
    overall_min = None
    overall_max = None
    group_counts = defaultdict(int)
    source_files = []

    for s in stats_list:
        source_files.append(s.get("file", "unknown"))

        c = int(s.get("count", 0))
        total_count += c
        total_sum += float(s.get("sum_value", 0.0))

        min_v = s.get("min_value")
        max_v = s.get("max_value")
        if min_v is not None:
            if overall_min is None or min_v < overall_min:
                overall_min = min_v
        if max_v is not None:
            if overall_max is None or max_v > overall_max:
                overall_max = max_v

        for g, gc in s.get("group_counts", {}).items():
            group_counts[g] += int(gc)

    mean_value = total_sum / total_count if total_count > 0 else None

    return {
        "source_files": source_files,
        "count": int(total_count),
        "sum_value": float(total_sum),
        "mean_value": mean_value,
        "min_value": overall_min,
        "max_value": overall_max,
        "group_counts": dict(group_counts),
    }


@delayed
def final_reduce(combined_list: List[Dict]) -> Dict:
    """
    FINAL REDUCE:
      - Takes a list of combined dicts
      - Aggregates everything into a final summary dict
    """
    total_count = 0
    total_sum = 0.0
    overall_min = None
    overall_max = None
    group_counts = defaultdict(int)
    all_files = []

    for s in combined_list:
        all_files.extend(s.get("source_files", []))

        c = int(s.get("count", 0))
        total_count += c
        total_sum += float(s.get("sum_value", 0.0))

        min_v = s.get("min_value")
        max_v = s.get("max_value")
        if min_v is not None:
            if overall_min is None or min_v < overall_min:
                overall_min = min_v
        if max_v is not None:
            if overall_max is None or max_v > overall_max:
                overall_max = max_v

        for g, gc in s.get("group_counts", {}).items():
            group_counts[g] += int(gc)

    mean_value = total_sum / total_count if total_count > 0 else None

    return {
        "num_input_files": len(set(all_files)),
        "all_input_files": sorted(set(all_files)),
        "total_count": int(total_count),
        "total_sum_value": float(total_sum),
        "mean_value": mean_value,
        "overall_min_value": overall_min,
        "overall_max_value": overall_max,
        "group_counts": dict(group_counts),
    }


# ─────────────────────────────────────────────────────────────────────────────
# Input discovery
# ─────────────────────────────────────────────────────────────────────────────

def discover_input_files(input_dir: str, extensions=("jsonl", "json", "csv")) -> List[Path]:
    base = Path(input_dir)
    if not base.is_dir():
        raise ValueError(f"Input directory {input_dir} does not exist or is not a directory")

    files: List[Path] = []
    for ext in extensions:
        files.extend(base.glob(f"*.{ext}"))
    files = sorted(files)

    if not files:
        raise ValueError(f"No input files with extensions {extensions} found in {input_dir}")

    return files


def chunk_list(seq, chunk_size: int):
    if chunk_size <= 0:
        return [seq]
    return [seq[i: i + chunk_size] for i in range(0, len(seq), chunk_size)]


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Map-Combine-Reduce benchmark")
    parser.add_argument("--scheduler", choices=["threads", "taskvine"],
                        default="threads")
    parser.add_argument("--port", type=int, default=MANAGER_PORT)
    parser.add_argument("--name", default=MANAGER_NAME)
    parser.add_argument("--input-dir", default=INPUT_DIR)
    parser.add_argument("--num-files", type=int, default=8,
                        help="Number of input files to generate (--generate) or use")
    parser.add_argument("--combine-width", type=int, default=2,
                        help="Number of map outputs to combine per combine task")
    parser.add_argument("--generate", action="store_true",
                        help="Generate synthetic input data and exit")
    args = parser.parse_args()

    if args.generate:
        import generate_input_data
        Path(args.input_dir).mkdir(parents=True, exist_ok=True)
        print("Generating synthetic input data...")
        generate_input_data.generate_input_data(
            base_dir=args.input_dir,
            num_files=args.num_files,
            records_per_file=RECORDS_PER_FILE,
            groups=GROUPS,
            group_means=GROUP_MEANS,
            group_stds=GROUP_STDS,
            seed=RNG_SEED,
        )
        print(f"Done. {args.num_files} files written to {args.input_dir!r}")
        return

    input_files = discover_input_files(args.input_dir)
    if args.num_files:
        input_files = input_files[:args.num_files]
    input_paths = [str(p.resolve()) for p in input_files]
    print(f"Using {len(input_paths)} input files.")

    # Build the Dask delayed graph
    stats_tasks   = [map_file_to_stats(path) for path in input_paths]
    grouped_stats = chunk_list(stats_tasks, args.combine_width)
    combine_tasks = [combine_stats_files(group) for group in grouped_stats]
    final_task    = final_reduce(combine_tasks)

    print(f"Graph: {len(stats_tasks)} map, {len(combine_tasks)} combine (width={args.combine_width}), 1 reduce.")

    start_time = time.time()

    if args.scheduler == "threads":
        print("Running with local threaded scheduler...")
        (final_summary,) = dask.compute(final_task, scheduler="threads")

    else:  # taskvine
        from ndcctools.taskvine.dask_executor import DaskVine
        print(f"Connecting to DaskVine manager name={args.name!r}, port={args.port}...")
        m = DaskVine(port=args.port, name=args.name)

        (final_summary,) = dask.compute(
            final_task,
            scheduler=m.get,
            resources={"cores": 1},
            resources_mode=None,
            worker_transfers=True,
            progress_disable=False,
        )

        m.workflow_summary()

    elapsed = time.time() - start_time
    print(f"Computation complete in {elapsed:.2f} seconds.")

    Path(OUTPUT_DIR).mkdir(parents=True, exist_ok=True)
    output_path = Path(OUTPUT_JSON)
    with output_path.open("w") as f:
        json.dump(final_summary, f, indent=2)

    print(f"\nFinal summary written to: {output_path.resolve()}")
    print(f"Total records: {final_summary['total_count']}")
    print(f"Mean value:    {final_summary['mean_value']}")


if __name__ == "__main__":
    main()
