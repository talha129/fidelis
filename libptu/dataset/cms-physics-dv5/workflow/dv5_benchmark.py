#!/usr/bin/env python3
"""
CMS Physics DV5 ECF Analysis benchmark.

Usage
-----
  # Preprocess data (scan ROOT files → samples_ready.json), then run:
  python dv5_benchmark.py --data-dir /shared/dv5-data/samples --preprocess

  # Run without re-preprocessing (samples_ready.json already exists):
  python dv5_benchmark.py --data-dir /shared/dv5-data/samples

  # Process a specific sub-dataset only:
  python dv5_benchmark.py --data-dir /shared/dv5-data/samples --sub-dataset diboson_zz

  # Process hgg_1 dataset with a fixed number of ROOT files:
  python dv5_benchmark.py --data-dir /shared/dv5-input-samples --preprocess \
      --sub-dataset hgg_1 --num-files 50

  # Custom manager name and ports:
  python dv5_benchmark.py --data-dir /shared/dv5-data/samples --name dv5-manager --ports 9123 9150

  # Enable warm-pool transparently via env var:
  TASKVINE_WARM_POOL=1 python dv5_benchmark.py --data-dir /shared/dv5-data/samples
"""

import argparse
import json
import os
import sys
import time
import warnings

import dask
from coffea import dataset_tools
from coffea.nanoevents import PFNanoAODSchema
from ndcctools.taskvine.compat import DaskVine

from ecf_helpers import preprocess_data, filter_existing_files, analysis


def main():
    parser = argparse.ArgumentParser(description="CMS Physics DV5 ECF Analysis benchmark")
    parser.add_argument("--data-dir", default="/shared/dv5-data/samples",
                        help="Directory containing sample ROOT files (default: /shared/dv5-data/samples)")
    parser.add_argument("--samples-ready", default="samples_ready.json",
                        help="Path to preprocessed samples JSON (default: samples_ready.json)")
    parser.add_argument("--preprocess", action="store_true",
                        help="Force re-preprocessing even if samples_ready.json exists")
    parser.add_argument("--step-size", type=int, default=50_000,
                        help="Events per chunk for preprocessing (default: 50000)")
    parser.add_argument("--ecf-upper-bound", type=int, default=3, choices=[3, 4, 5, 6],
                        help="Calculate ECFs from n=2 to n=ECF_UPPER_BOUND (default: 3)")
    parser.add_argument("--all", action="store_true",
                        help="Process all datasets (default: True)")
    parser.add_argument("--sub-dataset", type=str, default=None,
                        help="Process a single sub-dataset (e.g. diboson_zz, qcd_800to1000)")
    parser.add_argument("--num-files", type=int, default=None,
                        help="Limit each dataset to this many ROOT files (default: all)")
    parser.add_argument("--output-dir", default="/shared/dv5-output",
                        help="Base directory for output parquet files (default: /shared/dv5-output)")
    parser.add_argument("--name", default=os.environ.get("VINE_MANAGER_NAME", "dv5-manager"),
                        help="TaskVine manager name (default: $VINE_MANAGER_NAME or 'dv5-manager')")
    parser.add_argument("--ports", type=int, nargs="+",
                        default=[int(p.strip()) for p in
                                 os.environ.get("VINE_MANAGER_PORTS", "9123,9150").split(",")],
                        help="TaskVine manager ports (default: $VINE_MANAGER_PORTS or 9123 9150)")
    parser.add_argument("--run-info-path", default="vine-run-info",
                        help="Directory for TaskVine logs (default: vine-run-info)")
    parser.add_argument("--staging-path", default="/tmp/dv5-staging",
                        help="Temporary staging directory (default: /tmp/dv5-staging)")
    parser.add_argument("--triggers-file", default="triggers.json",
                        help="Path to triggers JSON (default: triggers.json)")
    args = parser.parse_args()

    warnings.filterwarnings("ignore", "Found duplicate branch")
    warnings.filterwarnings("ignore", "Missing cross-reference index for")
    warnings.filterwarnings("ignore", "dcut")
    warnings.filterwarnings("ignore", "Please ensure")
    warnings.filterwarnings("ignore", "invalid value")

    # ── Manager ───────────────────────────────────────────────────────────────
    m = DaskVine(
        args.ports,
        name=args.name,
        run_info_path=args.run_info_path,
        staging_path=args.staging_path,
    )
    m.tune("max-workers", 30000)
    m.tune("max-retrievals", 10)
    m.tune("transient-error-interval", 1)
    m.tune("worker-source-max-transfers", 10000)
    m.tune("transfer-temps-recovery", 0)
    m.tune("attempt-schedule-depth", 100)
    m.tune("watch-library-logfiles", 1)
    m.tune("temp-replica-count", 1)
    print(f"TaskVine manager: name={args.name!r}, ports={args.ports}")

    # ── Phase 1: Preprocess ───────────────────────────────────────────────────
    if args.preprocess or not os.path.exists(args.samples_ready):
        print(f"\nPhase 1: Preprocessing data from {args.data_dir} ...")
        t0 = time.time()
        samples_ready = preprocess_data(
            args.data_dir,
            step_size=args.step_size,
            manager=m,
            num_files=args.num_files,
        )
        with open(args.samples_ready, "w") as f:
            json.dump(samples_ready, f)
        print(f"Preprocessing complete in {(time.time()-t0)/60:.2f} min → {args.samples_ready}")
        m.workflow_summary(verbose=True)
    else:
        print(f"\nPhase 1: Loading preprocessed samples from {args.samples_ready}")
        with open(args.samples_ready) as f:
            samples_ready = json.load(f)
        print(f"Loaded {len(samples_ready)} sample categories")

    # ── Phase 2: Filter and select ────────────────────────────────────────────
    filtered = filter_existing_files(samples_ready)
    if not filtered:
        print("ERROR: No valid ROOT files found. Check --data-dir path.")
        sys.exit(1)

    print(f"\nAvailable datasets:")
    for name, info in filtered.items():
        print(f"  {name}: {len(info['files'])} files")

    if args.sub_dataset:
        if args.sub_dataset not in filtered:
            print(f"ERROR: sub-dataset '{args.sub_dataset}' not found. "
                  f"Available: {list(filtered.keys())}")
            sys.exit(1)
        samples_to_process = {args.sub_dataset: filtered[args.sub_dataset]}
    else:
        samples_to_process = filtered

    if args.num_files is not None:
        samples_to_process = {
            ds: {**info, "files": dict(list(info["files"].items())[:args.num_files])}
            for ds, info in samples_to_process.items()
        }

    print(f"\nProcessing {len(samples_to_process)} dataset(s):")
    for name, info in samples_to_process.items():
        print(f"  {name}: {len(info['files'])} files")

    # ── Phase 3: Build and run analysis ──────────────────────────────────────
    ecf_upper_bound = args.ecf_upper_bound
    triggers_file   = args.triggers_file
    output_dir      = args.output_dir

    def analysis_wrapper(events):
        return analysis(events,
                        ecf_upper_bound=ecf_upper_bound,
                        triggers_file=triggers_file,
                        output_dir=output_dir)

    print(f"\nPhase 3: Building analysis tasks (ECF n=2..{ecf_upper_bound}) ...")
    tasks = dataset_tools.apply_to_fileset(
        analysis_wrapper,
        samples_to_process,
        uproot_options={"allow_read_errors_with_report": False},
        schemaclass=PFNanoAODSchema,
    )

    print("Starting computation — waiting for workers ...")
    start_time = time.time()

    dask.compute(
        tasks,
        scheduler=m.get,
        resources_mode=None,
        prune_depth=0,
        worker_transfers=True,
        resources={"cores": 1},
    )

    elapsed = time.time() - start_time

    # ── Summary ───────────────────────────────────────────────────────────────
    print("\n" + "=" * 60)
    print(f"Computation complete in {elapsed:.2f}s ({elapsed/60:.2f} min)")
    print(f"Datasets processed: {len(samples_to_process)}")
    if os.path.exists(output_dir):
        print(f"\nOutput ({output_dir}/):")
        for item in sorted(os.listdir(output_dir)):
            item_path = os.path.join(output_dir, item)
            if os.path.isdir(item_path):
                n = len([f for f in os.listdir(item_path) if f.endswith(".parquet")])
                print(f"  {item}: {n} parquet files")

    m.workflow_summary(verbose=True)


if __name__ == "__main__":
    main()
