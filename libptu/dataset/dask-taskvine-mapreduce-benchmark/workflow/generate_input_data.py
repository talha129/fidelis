#!/usr/bin/env python3
"""
generate_input_data.py

Utility to generate synthetic CSV datasets for the
Dask + TaskVine Map–Combine–Reduce benchmark.

Each file contains rows of:
    id, group, value

Value generation can optionally depend on group-specific
Gaussian distributions.
"""

import csv
import random
import shutil
from pathlib import Path
from typing import List, Dict


def generate_input_data(
    base_dir: str,
    num_files: int,
    records_per_file: int,
    groups: List[str],
    group_means: Dict[str, float] = None,
    group_stds: Dict[str, float] = None,
    seed: int = 42,
    prefix: str = "input",
    remove_existing_data: bool = True,
):
    """
    Generate synthetic CSV files.

    Parameters
    ----------
    base_dir : str
        Output directory for generated data files.
    num_files : int
        Number of files to generate.
    records_per_file : int
        Number of rows per file.
    groups : list[str]
        Group labels.
    group_means : dict[str, float], optional
        Mean value per group (Gaussian). If None, use uniform.
    group_stds : dict[str, float], optional
        Std deviation per group. If None, use fixed std=1.
    seed : int
        Random seed for reproducibility.
    prefix : str
        Filename prefix (default: "input").
    remove_existing_data : bool
        If True, remove existing data directory before generating new files.
        Default is True to prevent task count from growing with repeated runs.
    """
    random.seed(seed)

    out_dir = Path(base_dir)
    
    # Remove existing data if flag is set
    if remove_existing_data and out_dir.exists():
        print(f"Removing existing data directory: {out_dir.resolve()}")
        shutil.rmtree(out_dir)
    
    out_dir.mkdir(parents=True, exist_ok=True)

    # Default means/stdevs if not provided
    if group_means is None:
        group_means = {g: 10.0 for g in groups}
    if group_stds is None:
        group_stds = {g: 1.0 for g in groups}

    for file_index in range(1, num_files + 1):
        filename = out_dir / f"{prefix}_{file_index:03d}.csv"

        with filename.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["id", "group", "value"])
            writer.writeheader()

            for row_index in range(records_per_file):
                g = random.choice(groups)

                # Gaussian-distributed synthetic value
                mean = group_means.get(g, 10.0)
                std = group_stds.get(g, 2.0)
                value = random.gauss(mean, std)

                writer.writerow({
                    "id": f"{file_index}-{row_index}",
                    "group": g,
                    "value": round(value, 6),
                })

        print(f"Created {filename} ({records_per_file} rows).")

    print(f"\nGenerated {num_files} files in {out_dir.resolve()}")


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="Generate synthetic CSV data for map-reduce benchmark")
    parser.add_argument("--output-dir", default="/shared/map-reduce-data",
                        help="Directory to write CSV files (default: /shared/map-reduce-data)")
    parser.add_argument("--num-files", type=int, default=8,
                        help="Number of files to generate (default: 8)")
    parser.add_argument("--records-per-file", type=int, default=5000,
                        help="Rows per file (default: 5000)")
    parser.add_argument("--groups", nargs="+", default=["A", "B", "C", "D"],
                        help="Group labels (default: A B C D)")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed (default: 42)")
    parser.add_argument("--prefix", default="input",
                        help="Filename prefix (default: input)")
    parser.add_argument("--no-remove-existing", action="store_true",
                        help="Keep existing files instead of wiping output dir first")
    args = parser.parse_args()

    generate_input_data(
        base_dir=args.output_dir,
        num_files=args.num_files,
        records_per_file=args.records_per_file,
        groups=args.groups,
        seed=args.seed,
        prefix=args.prefix,
        remove_existing_data=not args.no_remove_existing,
    )
