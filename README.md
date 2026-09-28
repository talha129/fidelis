# Fidelis: Exploiting Task Similarity for Low-overhead Distributed Workflow Audit and Replay

This repository is the artifact accompanying the Middleware 2026 submission
*"Fidelis: Exploiting Task Similarity for Low-overhead Distributed Workflow
Audit and Replay."* It contains the audit/replay system itself, the
modified TaskVine execution-context-reuse mechanism it depends on, and the
seven benchmark workflows and cluster orchestration scripts used to produce
the paper's evaluation.

**Badges requested:** Artifacts Available, Artifacts Functional, Results Reproduced.

## What's in this repository

| Directory | Contents |
|---|---|
| `provenance-to-use/` | PTU: the strace-based capture/replay tool Fidelis builds on. Source, build system, and test harness. |
| `libptu/` | Fidelis's LD_PRELOAD-based in-process interposition library — the **asynchronous materialization** variant (audit is on the task's critical path; container construction happens off-path after the worker exits). Includes `libptu-launcher`, `libptu-materialize`, and the seven benchmark workflows under `libptu/dataset/`. |
| `libptu-sync/` | The same interposition library, in its **synchronous materialization** variant (container is materialized before the task-equivalent-class first runs, not deferred). |
| `taskvine/` | TaskVine (from the [cctools](https://github.com/cooperative-computing-lab/cctools) suite), with execution context reuse added: `warm_manager.py` plus supporting changes to `manager.py`, `task.py`, and the Dask executors. Activated via the `TASKVINE_WARM_POOL` environment variable. |
| `cluster_driver_scripts/` | SLURM `sbatch` worker launchers and shell drivers used to run all six audit conditions (below) for every workflow and scale. |

### The seven benchmark workflows (`libptu/dataset/`)

| Directory | Workflow |
|---|---|
| `dask-taskvine-mapreduce-benchmark/` | MapReduce (word-count style, Dask-on-TaskVine) |
| `climate_trend/` | CTrend (climate anomaly/trend analysis) |
| `distributed_image_convolution/` | DConv (tiled image convolution) |
| `cms-physics-dv5/` | DV5 (CMS particle-physics event processing, DaskVine) |
| `rag-lite-bm25/` | RAG (document chunking + BM25 retrieval) |
| `minimap2_sv/` | Minimap2 long-read alignment (C tools: minimap2, samtools; hybrid, non-Python task bodies) |
| `gatk_hc/` | GATK HaplotypeCaller variant calling (Java/JVM; hybrid, non-Python task bodies) |

The last two are new additions beyond the paper's original five: each task
shells out to a compiled binary rather than calling a Python function,
exercising Fidelis's audit/replay path independent of the library-task
context-reuse mechanism (see Limitations below).

## The six audit conditions

Every workflow can be run under six conditions, isolating Fidelis's two
independent mechanisms (in-process interposition vs. ptrace, and execution
context reuse) and the choice of materialization timing:

| Condition | Worker wrapper | Manager env vars |
|---|---|---|
| Base Execution | none | none |
| Base Audit (ptrace) | `ptu` | none |
| Full Fidelis (async) | `libptu/libptu-launcher` | `TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1` |
| ptrace + reuse | `ptu` | `TASKVINE_WARM_POOL=1` |
| Interposition only (sync) | `libptu-sync/libptu-launcher` | `VINE_AUDIT_MODE=1` |
| Interposition + reuse (sync) | `libptu-sync/libptu-launcher` | `TASKVINE_WARM_POOL=1 VINE_AUDIT_MODE=1` |
| Fidelis Replay | `apptainer exec` into a previously captured `audit.sif` | `TASKVINE_WARM_POOL=1 VINE_REPLAY_MODE=1` |

Fidelis Replay is a seventh, separate condition: rather than auditing a
fresh execution, it replays a workflow entirely inside the SIF container
captured by a prior Full Fidelis run, with no new capture taking place.
See **Running Fidelis Replay** below.

## System requirements

- Linux (tested on Ubuntu 22.04), x86_64
- A SLURM cluster for the multi-node conditions above (tested on 8 nodes,
  2 vCPUs/node); a single machine is sufficient to exercise Fidelis itself
  at small scale, just without the SLURM orchestration layer. If you don't
  already have a SLURM cluster, the one used for this paper's evaluation
  was provisioned with [AWS ParallelCluster](https://docs.aws.amazon.com/parallelcluster/latest/ug/what-is-aws-parallelcluster.html),
  which sets up `sbatch`/`squeue` and shared storage automatically on EC2;
  a local [Slurm quick-start install](https://slurm.schedmd.com/quickstart_admin.html)
  works as well for a single-machine or bare-metal multi-node setup.
- [Miniconda/Miniforge](https://github.com/conda-forge/miniforge)
- Per-workflow tool dependencies, installed via conda:
  - `ndcctools` (TaskVine; see **Installing the modified TaskVine** below)
  - MapReduce/CTrend/DConv/DV5/RAG: `numpy`, `dask`, workflow-specific packages (`pandas`, `Pillow`, `awkward`/`uproot`/`coffea` for DV5, `langchain` for RAG)
  - Minimap2 workflow: `minimap2`, `samtools`
  - GATK workflow: `gatk4`, `bwa`, `samtools`
- A C toolchain to build `provenance-to-use`, `libptu`, and `libptu-sync`
  from source (Ubuntu/Debian: `apt-get install build-essential`). Not
  required if you use the prebuilt binaries — see **Building PTU and the
  interposition libraries**.
- [Apptainer](https://apptainer.org/docs/user/main/quick_start.html) —
  only needed for the **Fidelis Replay** condition (`apptainer exec` runs
  the workload inside a previously captured `audit.sif`). Not required
  for the other six conditions.

## Installing the modified TaskVine

The `taskvine/` directory in this repo contains only the Python bindings
subtree (`ndcctools.taskvine`) with the context-reuse patch applied, not a
full standalone build of cctools — the C-side worker/manager binaries are
unmodified upstream TaskVine, so there's no need to build cctools from
scratch.

**Version pin matters**: the patch is against `ndcctools==7.17.0`
specifically (`manager.py`/`task.py` internals it modifies can drift
between TaskVine releases) — install that exact version, not `latest`.
Every env created below pins it for you.

### Step 1: create each workflow's conda env

The driver scripts in `cluster_driver_scripts/` hardcode these exact env
names (`conda activate <name>`), so create them with these names —
anything else and the driver scripts will fail at `conda activate`.

| Workflow | Env name | Create with |
|---|---|---|
| MapReduce | `map-reduce` | `conda env create -f libptu/dataset/dask-taskvine-mapreduce-benchmark/software/environment.yml` |
| CTrend | `ctrend` | `conda env create -f libptu/dataset/climate_trend/software/environment.yml` |
| DConv | `dconv` | `conda env create -f libptu/dataset/distributed_image_convolution/software/environment.yml` |
| DV5 | `dv5` | `conda env create -f libptu/dataset/cms-physics-dv5/software/dv5-env.yml` |
| RAG | `rag` | `conda create -n rag -c conda-forge python=3.11 ndcctools=7.17.0 langchain-text-splitters -y` |
| Minimap2 | `minimap2-sv` | `conda create -n minimap2-sv -c conda-forge -c bioconda python=3.11 ndcctools=7.17.0 minimap2 samtools -y` |
| GATK | `gatk-hc` | `conda create -n gatk-hc -c conda-forge -c bioconda python=3.11 ndcctools=7.17.0 gatk4 bwa samtools -y` |

The four `environment.yml`-based envs already include a pinned
`ndcctools=7.17.0` alongside each workflow's own dependencies
(`pandas`/`numpy`/`matplotlib` for CTrend, `numpy`/`pillow` for DConv,
`dask`/`distributed` for MapReduce, `coffea`/`dask-awkward`/`awkward`/
`fastjet` plus a C toolchain for DV5's fastjet build).

### Step 2: overlay the warm-pool patch

Repeat this once per env created above, with that env active:

```bash
conda activate <env-name>   # e.g. ctrend, dconv, dv5, map-reduce, rag, minimap2-sv, gatk-hc
cp taskvine/taskvine/src/bindings/python3/ndcctools/taskvine/*.py \
   "$(python3 -c 'import ndcctools.taskvine, os; print(os.path.dirname(ndcctools.taskvine.__file__))')/"

# Verify the patch took effect:
python3 -c "import ndcctools.taskvine as vine; print(hasattr(vine, 'warm_manager'))"   # expect True
```

## Building PTU and the interposition libraries

Prebuilt Linux x86-64 binaries are included directly in this repo, built
and tested on Ubuntu 22.04:

| File | What it is |
|---|---|
| `provenance-to-use/ptu` | ptrace-based audit wrapper (Base Audit condition) |
| `libptu/libptu.so` | Async-materialization interposition library (LD_PRELOAD target) |
| `libptu/libptu-launcher` | Launches a process with `libptu.so` preloaded |
| `libptu/libptu-materialize` | Builds the SIF container from a captured manifest |
| `libptu-sync/libptu.so`, `libptu-sync/libptu-launcher`, `libptu-sync/libptu-materialize` | Same three, sync-materialization variant |

If your target machine matches that architecture/ABI, no build step is
required — just make sure the binaries are executable (`chmod +x`) after
cloning (git preserves the executable bit, but some hosting/zip pipelines
don't). Verify before relying on them:

```bash
for f in provenance-to-use/ptu libptu/libptu.so libptu/libptu-launcher \
         libptu/libptu-materialize libptu-sync/libptu.so \
         libptu-sync/libptu-launcher libptu-sync/libptu-materialize; do
    file "$f"        # expect: ELF 64-bit LSB ... x86-64 ...
done
libptu/libptu-launcher --help    # should print usage, not "permission denied" / "cannot execute"
```

To build from source instead (e.g. for a different distribution/arch, or
to verify the binaries yourself):

```bash
cd provenance-to-use && ./run.sh -r        # release build; produces ptu
cd ../libptu && make                       # produces libptu.so, libptu-launcher, libptu-materialize
cd ../libptu-sync && make                  # same three, sync variant
```

No dependencies beyond a standard C toolchain (`libptu`/`libptu-sync`
only use glibc/POSIX headers — no third-party libraries to install first).

## Data

Most workflows need no external data — MapReduce, Minimap2, and GATK
generate their own synthetic input via the one-time steps shown in
**Running a workflow**. The rest ship real input data directly in this
repo, under each workflow's `workflow/data/` (or, for DConv, alongside
the script):

| Workflow | Data included | Size | Source |
|---|---|---|---|
| CTrend | 80 real per-station weather CSVs + `data/csv_index.json` | ~22 MB | public weather-station records |
| DConv | `npp.jpg` | 16 MB | public NASA image |
| RAG | `alice.txt`, `frankenstein.txt`, `shakespeare_complete.txt`, `pg64317.txt` | ~6 MB | public-domain Project Gutenberg texts |
| DV5 | 4 CMS NanoAOD sample files (2 `qcd/800to1000`, 2 `diboson/zz`) + `samples_ready.json`, `triggers.json` | ~1 MB | CMS Open Data |

This is enough to run every workflow at **small scale** exactly as shown
in **Running a workflow**, with no download step.

**Known gap — DV5 at medium/large scale**: the paper's medium (20 files)
and large (60 files) DV5 scales draw from a different CMS sample subset
(`hgg_1`) that is not included in this repo (the bundled 4 files are only
enough for `qcd_800to1000`/`diboson_zz` at small scale). Reproducing DV5
beyond small scale requires sourcing additional `hgg_1` NanoAOD files
yourself and placing them under
`libptu/dataset/cms-physics-dv5/workflow/data/samples/hgg_1/`, matching
the directory structure of the samples already there, then re-running
`dv5_benchmark.py --preprocess --sub-dataset hgg_1 --num-files <20 or 60>`
to regenerate `samples_ready.json` for that subset.

Data location expected by each workflow's default arguments (override with
the corresponding flag if you place data elsewhere):

| Workflow | Flag | Default | Included? |
|---|---|---|---|
| MapReduce | `--input-dir` | `/shared/map-reduce-data` | generated via `--generate` |
| CTrend | `--data-source` | `data/csv_index.json` (relative to `workflow/`) | yes |
| DConv | `--images` | `npp.jpg` (relative to `workflow/`) | yes |
| DV5 | `--data-dir` | `/shared/dv5-data/samples` | small scale only, see above |
| RAG | `--data-dir` | `/shared/rag-data` (script's own `data/` dir also works, see script) | yes |
| Minimap2 | `--reference-dir`, `--data-dir` | none (explicit) | generated |
| GATK | `--reference-dir`, `--bam-dir`, `--intervals-dir` | none (explicit) | generated |

## Running an experiment (all six conditions, per workflow)

Every `cluster_driver_scripts/run_*.sh` script does the same thing: submit
the worker sbatch job for one condition, wait for it to reach `RUNNING`,
launch the manager with that condition's environment variables, wait for
it to finish, then cancel the worker job. This is the actual set of
scripts used to produce this study's results — no command construction
required, just point one at a scale and run it.

**Before running any of these**: replace the `<MANAGER_HOST>` and
`<MANAGER_INTERNAL_IP>` placeholders in the corresponding
`cluster_driver_scripts/*_workers_*.sbatch` files with your own manager
node's address, and adjust the hardcoded `/shared/...` paths to wherever
this repo lives on your cluster.

| Workflow | Small scale | Medium scale | Large scale |
|---|---|---|---|
| MapReduce | `run_mr_small_all4.sh` + `run_mr_small_sync.sh` | *(no unified script — run `run_mr_base_medium.sh`, `run_mr_base_audit_medium.sh`, `run_mr_fidelis_medium.sh`, `run_mr_ptrace_reuse_medium.sh` individually)* + `run_mr_libptu_sync.sh` | `run_mr_large_all4.sh` + `run_mr_large_sync.sh` |
| CTrend | `run_ctrend_small_all4.sh` + `run_ctrend_small_sync.sh` | `run_ctrend_ablation_medium.sh` + `run_ctrend_libptu_sync.sh` | `run_ctrend_large_all4.sh` + `run_ctrend_large_sync.sh` |
| DConv | `run_dconv_small_all4.sh` + `run_dconv_small_sync.sh` | `run_dconv_ablation_medium.sh` + `run_dconv_libptu_sync.sh` | `run_dconv_large_all4.sh` + `run_dconv_large_sync.sh` |
| DV5 | `run_dv5_small_all4.sh` + `run_dv5_small_sync.sh` | `run_dv5_local_staging_all4.sh` + `run_dv5_libptu_sync.sh` | `run_dv5_large_all4.sh` + `run_dv5_large_sync.sh` |
| RAG | `run_rag_small_all4.sh` + `run_rag_small_sync.sh` | `run_rag_local_staging_all4.sh` + `run_rag_libptu_sync.sh` | `run_rag_large_all4.sh` + `run_rag_large_sync.sh` |
| Minimap2 | `run_mm2_small_smoke.sh` (Base Execution only) | `run_mm2_medium_all6.sh` (all 6 conditions in one script) | not run in this study |
| GATK | not run in this study | `run_gatk_medium_all6.sh` (conditions 1-4) + `run_gatk_sync_remaining.sh` (conditions 5-6) | not run in this study |

`*_all4` scripts cover Base Execution, Base Audit, Full Fidelis, and
ptrace+reuse; `*_sync`/`*_libptu_sync` scripts cover Interposition Only
and Interposition + Reuse; `*_all6` scripts cover all six in one run.
MapReduce's medium scale is the one gap in this study's own script
history — it was run condition-by-condition rather than through a single
driver; the four scripts listed cover the same four conditions the
`*_all4` scripts automate elsewhere.

For a single workflow/condition/scale without going through a full driver
script (e.g. to debug one condition in isolation), see the per-workflow
example commands below and combine them with the worker wrapper and
manager environment variables from the condition table above.

## Running one workflow manually

### MapReduce (`dask-taskvine-mapreduce-benchmark/`)

```bash
cd libptu/dataset/dask-taskvine-mapreduce-benchmark/workflow
python3 mapreduce_benchmark.py --generate --input-dir /shared/map-reduce-data --num-files 1024   # one-time
python3 mapreduce_benchmark.py --scheduler taskvine --port 9123 --name mr-base \
    --input-dir /shared/map-reduce-data --num-files 1024 --combine-width 2
```

### CTrend (`climate_trend/`)

```bash
cd libptu/dataset/climate_trend/workflow
python3 climate_trend_benchmark.py --name ctrend-base --ports 9123 9150 \
    --num-files 1500 --data-source data/csv_index.json --output-dir /shared/ctrend-data
```

### DConv (`distributed_image_convolution/`)

```bash
cd libptu/dataset/distributed_image_convolution/workflow
python3 image_convolution_benchmark.py --name dconv-base --ports 9123 9150 \
    --images npp.jpg --kernels sharpen --tile-size 256 --output-dir output
```

### DV5 (`cms-physics-dv5/`)

```bash
cd libptu/dataset/cms-physics-dv5/workflow
python3 dv5_benchmark.py --preprocess --sub-dataset hgg_1 --num-files 20   # one-time: caches samples_ready.json
python3 dv5_benchmark.py --name dv5-base --ports 9123 9150 \
    --sub-dataset hgg_1 --num-files 20 --samples-ready samples_ready.json \
    --data-dir /shared/dv5-data/samples --output-dir /shared/dv5-output
```

### RAG (`rag-lite-bm25/`)

```bash
cd libptu/dataset/rag-lite-bm25/workflow
python3 rag_benchmark.py --name rag-base --ports 9123 9150 \
    --num-files 2500 --data-dir /shared/rag-data --chunk-size 1000 --skip-query
```

### Minimap2 (`minimap2_sv/`)

```bash
cd libptu/dataset/minimap2_sv/workflow
python3 02_generate_reference.py --output-dir reference                          # one-time
python3 03_generate_long_reads.py --reference reference/genome.fa \
    --annotations reference/genome_annotations.bed --output-dir data --windows 30  # one-time
minimap2 -x map-ont -d reference/genome.mmi reference/genome.fa                  # one-time
python3 minimap2_benchmark.py --scheduler taskvine --name mm2-base \
    --reference-dir reference --data-dir data --cores-per-task 2 --ports 9123 9150
```

### GATK (`gatk_hc/`)

```bash
cd libptu/dataset/gatk_hc/workflow
source ~/gatk_env.sh                              # sets $GATK; see 00_install.sh in the original workflow drop
python3 01_generate_reference.py                  # one-time
bash 02_index_reference.sh                        # one-time, requires $GATK
python3 03_generate_reads.py                      # one-time
bash 04_align_and_index.sh                        # one-time, requires $GATK, bwa, samtools
python3 05_generate_intervals.py --intervals-per-chrom 1   # one-time
python3 gatk_benchmark.py --scheduler taskvine --name gatk-base \
    --reference-dir reference --bam-dir bam --intervals-dir intervals \
    --cores-per-task 2 --heap 2g --ports 9123 9150
```

Each benchmark script accepts `--name`/`--ports` (or reads
`VINE_MANAGER_NAME`/`VINE_MANAGER_PORTS`) to connect to the manager
started by the matching driver script; scale (task count) is controlled by
`--num-files` / `--windows` / `--intervals-per-chrom` as shown above —
increase these for larger-scale runs.

## Running Fidelis Replay

Fidelis Replay runs a workflow's manager and workers entirely inside the
`audit.sif` containers captured during a prior Full Fidelis (async) run,
using `apptainer exec` with `VINE_REPLAY_MODE=1` instead of
`VINE_AUDIT_MODE=1` — no new interposition/capture happens, the goal is to
verify the previously captured environment reproduces the run.

`cluster_driver_scripts/` includes:

- `replay_manager_only.sh` — replays just the manager inside its captured
  SIF, against unaudited workers you start separately. This is the
  reliable path: every Full Fidelis run in this study produced a
  manager-side `audit.sif`.
- `replay_full.sbatch` + `run_replay_full.sh` — replays *both* the manager
  and the workers inside their captured SIFs, matching the original
  design (`ablation_study_plan.md` §9.2 in our internal notes). This
  requires a **worker-side** `audit.sif` as well.

**Worker-side replay caveat:** a worker only produces its own `audit.sif`
if it reaches its `LIBPTU_BUILD_ON_EXIT` materialize handler before
exiting. Our 8-node SLURM driver scripts (`cluster_driver_scripts/*_workers_fidelis*.sbatch`,
used for every workflow's medium-scale ablation run in this study,
including the two new hybrid workflows) `scancel` the worker job as soon
as the manager finishes — which is *before* that handler runs. As a
result, those runs' worker directories are empty and only manager-side
replay (`replay_manager_only.sh`) is possible for them. Genuine per-worker
captures do exist from this project's original, non-ablation two-node
deployment; run `run_replay_full.sh` against those. To capture your own
worker-side SIF, remove or delay the `scancel` call in a `*_workers_fidelis*`
run and let the worker process exit on its own after the manager
completes.

## Interpreting output

Each condition's manager prints a completion summary (`Computation complete
in N seconds`) and writes a JSON summary with task counts. Per-task timing
is available from TaskVine's own performance log
(`vine-run-info/*/vine-logs/performance`, `time_execute_good` field) for
workflows that don't expose it directly (DV5, and the two hybrid
workflows). A successful run has `tasks_successful` equal to the expected
task count and zero `tasks_failed`.

## Reproducing the paper's results

This repository does not include the specific run-by-run results, the
ablation-study tracking spreadsheet, or the paper's figure-generation
scripts — reproduction means re-running the conditions above yourself and
comparing against the numbers published in the paper (Sections 5.2.1-5.2.3,
Figures 6-8), not diffing against a bundled reference CSV. Concretely:

1. For each of the five original workflows (`dask-taskvine-mapreduce-benchmark`,
   `climate_trend`, `distributed_image_convolution`, `cms-physics-dv5`,
   `rag-lite-bm25`), run all six audit conditions at the paper's stated
   scales (task counts are documented per workflow/scale in each
   workflow's own generator script).
2. Record each condition's Workflow Time, Avg Task Time, and Throughput
   as described in **Interpreting output**.
3. Compare against the paper: Fidelis audit should reduce workflow time by
   roughly the percentages stated in Section 5.2.1 relative to base
   execution and base audit, and per-task time and throughput should show
   the corresponding improvements in Sections 5.2.2-5.2.3. Exact figures
   will vary run-to-run (see the single-run-measurement caveat below);
   the relative ordering and rough magnitude across conditions is the
   claim to check.

## Known limitations

- Figures in the paper average over repeated runs are not available;
  single-run measurements were used throughout.
- Execution context reuse (`TASKVINE_WARM_POOL`) provides negligible
  benefit for the Minimap2 and GATK workflows, since their tasks shell out
  to compiled binaries rather than making Python function calls — this is
  a real, expected result of the mechanism's design (its module-signature
  matching operates over Python bytecode), not a bug.
- The elastic SLURM cluster used for the paper's evaluation is not
  included; `cluster_driver_scripts/` assumes SLURM (`sbatch`/`squeue`) is
  available and configured on your own cluster — see **System
  requirements** above for how to provision one (AWS ParallelCluster or a
  local Slurm install).
