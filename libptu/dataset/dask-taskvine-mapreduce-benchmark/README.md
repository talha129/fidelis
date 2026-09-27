# Dask + TaskVine Map–Reduce Benchmark

This benchmark evaluates **Dask** running on top of **TaskVine** using a
simple **map–reduce workflow**.

- **Tasks** = map, combine, reduce operations
- **Scheduler** = TaskVine (via `DaskVine`)
- **Two versions**: TaskVine-managed data transfer vs. shared filesystem

The benchmark is intentionally simple and generic so you can scale the
number of files and tasks and swap in different data formats or metrics.

---

## Contents

### `workflow/` - Jupyter Notebooks and Scripts

- **`dask-taskvine-mapreduce.ipynb`**  
  Interactive notebook using **TaskVine-managed data transfer** (no shared filesystem required)

- **`dash-taskvine-mapreduce-shared-fs.ipynb`**  
  Interactive notebook using **shared filesystem** for intermediate files

- **`generate_input_data.py`**  
  Helper module to generate synthetic CSV/JSON input files

### `software/` - Environment Configuration

- **`environment.yml`**  
  Conda environment with Dask, TaskVine, JupyterLab, and dependencies

### `compute/` - Compute Configuration

- **`compute.yml`**  
  Compute resource configuration

---

## To Run the Benchmark Locally

### 1. Create the Conda Environment

```bash
conda env create -f software/environment.yml
conda activate taskvine-dask
```

This installs:
- Python 3.11
- Dask and Distributed
- TaskVine (ndcctools)
- JupyterLab
- matplotlib, numpy, graphviz

### 2. Start JupyterLab

In one terminal:

```bash
conda activate taskvine-dask
jupyter lab workflow
```

This will open JupyterLab in your browser.

### 3. Start TaskVine Workers

In **another terminal**, you have two options:

**Option 1: Using vine_factory (multiple workers)**
```bash
conda activate taskvine-dask
vine_factory --manager-name dask-taskvine-mapreduce-manager --cores 1 --max-workers 8 --batch-type local
```

**Option 2: Using vine_worker (single worker)**
```bash
conda activate taskvine-dask
vine_worker localhost 9123
```

Leave this running. Workers will connect to the TaskVine manager when you run the notebook.

### 4. Run the Notebook

Open either:
- **`dask-taskvine-mapreduce.ipynb`** (TaskVine-managed data - recommended)
- **`dash-taskvine-mapreduce-shared-fs.ipynb`** (shared filesystem version)

Execute the cells in order. The notebook will:
1. Generate synthetic input data
2. Build a Dask graph for map-combine-reduce
3. Execute on TaskVine workers
4. Display results and visualizations

---

## Workflow Overview

The workflow looks like this:

```text
[input_000.csv]   [input_001.csv]   ...  [input_N.csv]
       |                 |                       |
   (map task)        (map task)              (map task)
       |                 |                       |
    [stats_dict]      [stats_dict]          [stats_dict]
       \\             //      \\                   //
        \\           //        \\                 //
         (combine tasks over groups of stats)
                          |
                [combined_dict] ...
                          |
                       (reduce)
                          |
               [final_summary.json]
```

**TaskVine-managed version**: Intermediate data (`stats_dict`, `combined_dict`) is managed by TaskVine and transferred between workers as needed.

**Shared filesystem version**: Functions write to disk (JSON files) and return file paths.

---

## Notebook Configuration

You can adjust these parameters in the notebook's configuration cell:

- **`NUM_FILES`**: Number of input files to generate (default: 8)
- **`RECORDS_PER_FILE`**: Rows per input file (default: 5,000)
- **`COMBINE_WIDTH`**: How many map outputs to merge per combine task (default: 2)
- **`USE_THREADED_SCHEDULER`**: Set to `True` for local testing without TaskVine
- **`MANAGER_NAME`**: TaskVine manager name (must match in `vine_factory`)

---

## Running on a Cluster

### Prerequisites

All terminals mentioned below must be **cluster terminals** (SSH sessions to the cluster headnode).

### 1. Set Up SSH Tunnel

If the notebook is running on a cluster headnode and you want to access JupyterLab from your local machine, create an SSH tunnel:

**On your local machine:**
```bash
ssh -L 8888:localhost:8888 username@cluster-headnode.domain
```

This forwards port 8888 from the cluster to your local machine. Adjust the port number if needed.

### 2. Start JupyterLab on Cluster

**In a cluster terminal:**
```bash
conda activate taskvine-dask
cd /path/to/dask-taskvine-mapreduce-benchmark
jupyter lab workflow --no-browser --port=8888
```

The `--no-browser` flag prevents the cluster from trying to open a browser. Copy the URL with the token and paste it into your local browser.

### 3. Start TaskVine Workers with Batch System

**In another cluster terminal**, use `vine_factory` with your cluster's batch system:

**For Condor:**
```bash
conda activate taskvine-dask
vine_factory --manager-name generic-mapreduce-manager --cores 4 --max-workers 32 --batch-type condor
```

**For Slurm:**
```bash
conda activate taskvine-dask
vine_factory --manager-name generic-mapreduce-manager --cores 4 --max-workers 32 --batch-type slurm
```

The `--batch-type` option tells `vine_factory` to submit workers as batch jobs to your cluster's job scheduler.

### 4. Run the Notebook

Access JupyterLab through your local browser using the forwarded port (e.g., `http://localhost:8888`), and run the notebook as usual.

### Notes for Cluster Usage

- **Shared filesystem**: The shared filesystem version (`dash-taskvine-mapreduce-shared-fs.ipynb`) works well on clusters with shared storage across nodes
- **Port conflicts**: If port 8888 is in use, choose a different port and update both the SSH tunnel and JupyterLab commands
- **Firewall rules**: Ensure TaskVine communication ports (default 9123) are open between the headnode and compute nodes
- **Resource limits**: Adjust `--cores` and `--max-workers` based on your cluster's resource policies

---

## Troubleshooting

### Workers not connecting?

Make sure:
- Workers are running with the correct `--manager-name`
- Manager name matches between notebook and `vine_factory`
- Both manager and workers are on the same network

### "No input files found"?

The notebook generates input data automatically in the first cells. Make sure to run all cells in order.

### Slow execution with local scheduler?

Set `USE_THREADED_SCHEDULER = True` in the notebook config for quick local testing without TaskVine workers.

---

## Scaling the Benchmark

**More files = more map tasks:**
```python
NUM_FILES = 100
RECORDS_PER_FILE = 10_000
```

**More workers:**
```bash
vine_factory --manager-name generic-mapreduce-manager --cores 4 --max-workers 32
```

**Adjust combine width:**
```python
COMBINE_WIDTH = 5  # Merge 5 map outputs per combine task
```

---

## Visualizations

The notebooks include:
- **Group counts bar chart**: Shows distribution of records across groups
- **TaskVine task graph**: Use `vine_plot_taskgraph` to visualize task dependencies

