#!/usr/bin/env python3
"""
Climate Trend Analysis benchmark.

Usage
-----
  python climate_trend_benchmark.py --num-files 10
  python climate_trend_benchmark.py --num-files 50 --name ctrend --ports 9123 9150
  python climate_trend_benchmark.py --num-files 50 --data-source data/csv_index.json

  # Enable warm-pool transparently via env var (no application change):
  TASKVINE_WARM_POOL=1 python climate_trend_benchmark.py --num-files 50 --ports 9123 9150
"""

import argparse
import json
import os
import time
from pathlib import Path

import ndcctools.taskvine as vine
from tqdm import tqdm


# ─────────────────────────────────────────────────────────────────────────────
# Processing functions (serialized into PythonTask — keep self-contained)
# ─────────────────────────────────────────────────────────────────────────────

def load_and_clean_data_from_url(csv_url, output_file):
    """Load CSV from URL, convert tenths→°C, save cleaned CSV."""
    import numpy as np
    import pandas as pd

    try:
        df = pd.read_csv(csv_url)
        df['DATE'] = pd.to_datetime(df['DATE'])

        temp_cols = ['TAVG', 'TMAX', 'TMIN']
        available_temp_cols = [col for col in temp_cols if col in df.columns]

        if not available_temp_cols:
            return {'file': output_file, 'error': 'No temperature columns found'}

        for col in available_temp_cols:
            df[col] = df[col] / 10.0

        primary_temp_col = 'TAVG' if 'TAVG' in df.columns else available_temp_cols[0]
        df = df.dropna(subset=[primary_temp_col])

        station_id = df['STATION'].iloc[0] if 'STATION' in df.columns and len(df) > 0 else 'Unknown'

        df.to_csv(output_file, index=False)

        return {
            'file': output_file,
            'station_id': station_id,
            'rows': len(df),
            'temp_columns': available_temp_cols,
            'date_range': [str(df['DATE'].min()), str(df['DATE'].max())] if len(df) > 0 else None,
        }
    except Exception as e:
        return {'file': output_file, 'error': str(e)}


def calculate_monthly_anomalies(input_file, output_file, baseline_start, baseline_end):
    """Calculate temperature anomalies relative to a baseline period."""
    import numpy as np
    import pandas as pd

    try:
        df = pd.read_csv(input_file)
        df['DATE'] = pd.to_datetime(df['DATE'])

        temp_col = 'TAVG' if 'TAVG' in df.columns else ('TMAX' if 'TMAX' in df.columns else 'TMIN')
        if temp_col not in df.columns:
            return {'file': input_file, 'error': 'No temperature column found'}

        df['MONTH'] = df['DATE'].dt.month
        df['YEAR'] = df['DATE'].dt.year

        baseline_data = df[(df['YEAR'] >= baseline_start) & (df['YEAR'] <= baseline_end)]
        if len(baseline_data) == 0:
            return {'file': input_file, 'error': f'No data in baseline period {baseline_start}-{baseline_end}'}

        baseline_monthly = baseline_data.groupby('MONTH')[temp_col].mean().to_dict()

        df['TEMP_ANOMALY'] = df.apply(
            lambda row: row[temp_col] - baseline_monthly.get(row['MONTH'], np.nan), axis=1
        )

        result_df = df[['DATE', 'YEAR', 'MONTH', temp_col, 'TEMP_ANOMALY']].copy()
        result_df.to_csv(output_file, index=False)

        return {
            'file': input_file,
            'processed_records': len(result_df),
            'baseline_years': len(baseline_data['YEAR'].unique()),
            'anomaly_stats': {
                'mean': float(result_df['TEMP_ANOMALY'].mean()),
                'std': float(result_df['TEMP_ANOMALY'].std()),
                'min': float(result_df['TEMP_ANOMALY'].min()),
                'max': float(result_df['TEMP_ANOMALY'].max()),
            },
        }
    except Exception as e:
        return {'file': input_file, 'error': str(e)}


def calculate_annual_trends(input_files, output_file):
    """Aggregate anomaly files and compute linear regression trend."""
    import numpy as np
    import pandas as pd
    from scipy import stats

    try:
        all_data = []
        for input_file in input_files:
            df = pd.read_csv(input_file)
            if len(df) > 0:
                all_data.append(df)

        if not all_data:
            return {'error': 'No valid data files to process'}

        combined_df = pd.concat(all_data, ignore_index=True)
        combined_df['DATE'] = pd.to_datetime(combined_df['DATE'])

        annual_data = combined_df.groupby('YEAR').agg(
            {'TEMP_ANOMALY': ['mean', 'std', 'count']}
        ).round(4)
        annual_data.columns = ['ANNUAL_ANOMALY', 'ANNUAL_STD', 'RECORD_COUNT']
        annual_data = annual_data.reset_index()

        years = annual_data['YEAR'].values
        anomalies = annual_data['ANNUAL_ANOMALY'].values
        valid_idx = ~np.isnan(anomalies)

        if np.sum(valid_idx) > 2:
            slope, intercept, r_value, p_value, std_err = stats.linregress(
                years[valid_idx], anomalies[valid_idx]
            )
            annual_data['TREND_LINE'] = slope * years + intercept
            trend_info = {
                'slope': slope,
                'intercept': intercept,
                'r_squared': r_value ** 2,
                'p_value': p_value,
                'trend_per_decade': slope * 10,
                'trend_significance': 'significant' if p_value < 0.05 else 'not significant',
            }
        else:
            trend_info = {'error': 'Insufficient data for trend calculation'}

        annual_data.to_csv(output_file, index=False)

        return {
            'processed_years': len(annual_data),
            'year_range': [int(annual_data['YEAR'].min()), int(annual_data['YEAR'].max())],
            'stations_processed': len(input_files),
            'trend_info': trend_info,
            'mean_anomaly': float(annual_data['ANNUAL_ANOMALY'].mean()),
            'total_records': int(annual_data['RECORD_COUNT'].sum()),
        }
    except Exception as e:
        return {'error': str(e)}


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description="Climate Trend Analysis benchmark")
    parser.add_argument("--num-files", type=int, default=50,
                        help="Max number of station files to process (default: 50)")
    parser.add_argument("--data-source", default="data/csv_index.json",
                        help="Path to JSON index of station URLs (default: data/csv_index.json)")
    parser.add_argument("--name", default=os.environ.get("VINE_MANAGER_NAME", "ctrend"),
                        help="TaskVine manager name (default: $VINE_MANAGER_NAME or 'ctrend')")
    parser.add_argument("--ports", type=int, nargs="+",
                        default=[int(p.strip()) for p in
                                 os.environ.get("VINE_MANAGER_PORTS", "9123,9150").split(",")],
                        help="TaskVine manager ports (default: $VINE_MANAGER_PORTS or 9123 9150)")
    parser.add_argument("--output-dir", default="/shared/ctrend-data",
                        help="Shared directory for intermediate and final output files (default: /shared/ctrend-data)")
    args = parser.parse_args()

    data_source = args.data_source
    if not Path(data_source).exists():
        print(f"ERROR: data source not found: {data_source}")
        raise SystemExit(1)

    output_dir = args.output_dir
    Path(output_dir).mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {output_dir}")

    with open(data_source) as f:
        stations = json.load(f)
    if args.num_files and len(stations) > args.num_files:
        stations = stations[:args.num_files]
    print(f"Stations to process: {len(stations)}")

    m = vine.Manager(port=args.ports, name=args.name)
    m.tune("watch-library-logfiles", 1)
    print(f"TaskVine manager: name={args.name!r}, ports={args.ports}")

    start_time = time.time()
    baseline_start, baseline_end = 1991, 2020

    # ── Phase 1: load and clean ───────────────────────────────────────────────
    print("\nPhase 1: Loading and cleaning station data...")
    cleaning_tasks = {}
    for station in stations:
        output_file = f"{output_dir}/cleaned_{station['name']}"
        task = vine.PythonTask(load_and_clean_data_from_url, station['url'], output_file)
        task.set_cores(1)
        task_id = m.submit(task)
        cleaning_tasks[task_id] = {'station': station['name'], 'output': output_file}

    cleaned_files = []
    cleaning_results = []
    with tqdm(total=len(cleaning_tasks), desc="Cleaning") as pbar:
        while cleaning_tasks:
            task = m.wait(5)
            if task is None:
                continue
            info = cleaning_tasks.pop(task.id, {})
            try:
                result = task.output
            except Exception as exc:
                result = {'error': str(exc)}
            cleaning_results.append(result)
            if 'error' in result:
                print(f"  FAIL {info.get('station', 'unknown')}: {result['error']}")
            else:
                cleaned_files.append(info['output'])
            pbar.update(1)

    print(f"Phase 1 complete: {len(cleaned_files)}/{len(stations)} files cleaned")

    if not cleaned_files:
        print("ERROR: No files cleaned. Exiting.")
        raise SystemExit(1)

    # ── Phase 2: monthly anomalies ────────────────────────────────────────────
    print("\nPhase 2: Calculating monthly anomalies...")
    anomaly_tasks = {}
    for cleaned_file in cleaned_files:
        anomaly_file = f"{output_dir}/anomalies_{Path(cleaned_file).name}"
        task = vine.PythonTask(
            calculate_monthly_anomalies,
            cleaned_file,
            anomaly_file,
            baseline_start,
            baseline_end,
        )
        task.set_cores(1)
        task_id = m.submit(task)
        anomaly_tasks[task_id] = {'input': cleaned_file, 'output': anomaly_file}

    valid_anomaly_files = []
    anomaly_results = []
    with tqdm(total=len(anomaly_tasks), desc="Anomalies") as pbar:
        while anomaly_tasks:
            task = m.wait(5)
            if task is None:
                continue
            info = anomaly_tasks.pop(task.id, {})
            try:
                result = task.output
            except Exception as exc:
                result = {'error': str(exc)}
            anomaly_results.append(result)
            if 'error' in result:
                print(f"  FAIL anomaly: {result['error']}")
            else:
                valid_anomaly_files.append(info['output'])
            pbar.update(1)

    print(f"Phase 2 complete: {len(valid_anomaly_files)} anomaly files created")

    if not valid_anomaly_files:
        print("ERROR: No anomaly files created. Exiting.")
        raise SystemExit(1)

    # ── Phase 3: annual trends ────────────────────────────────────────────────
    print("\nPhase 3: Calculating annual trends...")
    trends_output = f"{output_dir}/annual_trends_large.csv"
    trend_task = vine.PythonTask(calculate_annual_trends, valid_anomaly_files, trends_output)
    trend_task.set_cores(1)
    trend_task_id = m.submit(trend_task)

    trend_result = None
    while True:
        task = m.wait(5)
        if task is None:
            continue
        if task.id == trend_task_id:
            try:
                trend_result = task.output
            except Exception as exc:
                trend_result = {'error': str(exc)}
            break

    elapsed = time.time() - start_time

    if trend_result and 'error' in trend_result:
        print(f"ERROR trend calculation: {trend_result['error']}")
        raise SystemExit(1)

    # ── Summary ───────────────────────────────────────────────────────────────
    print("\n" + "=" * 60)
    print(f"Computation complete in {elapsed:.2f} seconds.")
    print(f"Stations processed:  {trend_result['stations_processed']}")
    print(f"Analysis period:     {trend_result['year_range'][0]}-{trend_result['year_range'][1]} "
          f"({trend_result['processed_years']} years)")
    print(f"Total records:       {trend_result['total_records']:,}")
    print(f"Mean anomaly:        {trend_result['mean_anomaly']:.3f} C")

    trend_info = trend_result.get('trend_info', {})
    if 'slope' in trend_info:
        print(f"Trend:               {trend_info['trend_per_decade']:.4f} C/decade "
              f"({trend_info['trend_significance']})")
        print(f"R-squared:           {trend_info['r_squared']:.3f}")
        print(f"P-value:             {trend_info['p_value']:.2e}")

    print(f"Results saved to:    {trends_output}")

    m.workflow_summary()


if __name__ == "__main__":
    main()
