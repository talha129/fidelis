# Fidelis: Exploiting Task Similarity for Low-overhead Distributed Workflow Audit and Replay

This repository is the artifact accompanying the Middleware 2026 submission
*"Fidelis: Exploiting Task Similarity for Low-overhead Distributed Workflow
Audit and Replay."* It contains the audit/replay system itself, the
modified TaskVine execution-context-reuse mechanism it depends on, and the
seven benchmark workflows and cluster orchestration scripts used to produce
the paper's evaluation.

**Badges requested:** Artifacts Available, Artifacts Functional.

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
  at small scale, just without the SLURM orchestration layer
- [Miniconda/Miniforge](https://github.com/conda-forge/miniforge)
- Per-workflow tool dependencies, installed via conda:
  - `ndcctools` (TaskVine; see **Installing the modified TaskVine** below)
  - MapReduce/CTrend/DConv/DV5/RAG: `numpy`, `dask`, workflow-specific packages (`pandas`, `Pillow`, `awkward`/`uproot`/`coffea` for DV5, `langchain` for RAG)
  - Minimap2 workflow: `minimap2`, `samtools`
  - GATK workflow: `gatk4`, `bwa`, `samtools`
- A C toolchain (gcc/make) to build `provenance-to-use`, `libptu`, and `libptu-sync`

## Installing the modified TaskVine

The `taskvine/` directory in this repo contains only the Python bindings
subtree (`ndcctools.taskvine`) with the context-reuse patch applied, not a
full standalone build of cctools. The tested path is:

```bash
conda install -c conda-forge ndcctools=7.17.0   # installs stock TaskVine (C binaries + Python bindings)
# then overlay the modified Python files from this repo onto the installed package:
cp taskvine/taskvine/src/bindings/python3/ndcctools/taskvine/*.py \
   "$(python3 -c 'import ndcctools.taskvine, os; print(os.path.dirname(ndcctools.taskvine.__file__))')/"
```

This replaces `manager.py`, `task.py`, `dask_executor.py`,
`compat/dask_executor.py`, `__init__.py`, and adds `warm_manager.py`, while
reusing the conda package's compiled `vine_worker`/`vine_manager` binaries
unmodified — the context-reuse mechanism is entirely on the Python side.

## Building PTU and the interposition libraries

Prebuilt Linux x86-64 binaries are included directly in this repo
(`provenance-to-use/ptu`, `libptu/libptu-launcher`,
`libptu/libptu-materialize`, `libptu-sync/libptu-launcher`,
`libptu-sync/libptu-materialize`), built and tested on Ubuntu 22.04. If
your target machine matches that architecture/ABI, no build step is
required — just make sure the binaries are executable (`chmod +x`) after
cloning.

To build from source instead (e.g. for a different distribution or to
verify the binaries yourself):

```bash
cd provenance-to-use && ./run.sh -r        # release build
cd ../libptu && make
cd ../libptu-sync && make
```

Each produces a `libptu-launcher` and `libptu-materialize` binary used by
the worker-wrapper commands in the table above.

## Running a workflow

1. Create a workflow-specific conda env (see **System requirements**) with
   `ndcctools` patched per **Installing the modified TaskVine**.
2. Launch workers on your cluster using the matching `cluster_driver_scripts/*_workers_*.sbatch`
   file for the condition you want. **Before running**: replace
   `<MANAGER_HOST>` and `<MANAGER_INTERNAL_IP>` placeholders in the sbatch
   files with your own manager node's address, and adjust the hardcoded
   `/shared/...` paths (NFS-mounted shared storage in our deployment) to
   wherever `libptu-launcher`/`libptu-sync`'s launcher lives on your
   cluster.
3. Run the workflow's benchmark script from `libptu/dataset/<workflow>/workflow/`,
   e.g.:
   ```bash
   python3 minimap2_benchmark.py --scheduler taskvine \
       --reference-dir <ref-dir> --data-dir <data-dir> --cores-per-task 2 --ports 9123 9150
   ```
   Each benchmark script accepts `--name`/`--ports` (or reads
   `VINE_MANAGER_NAME`/`VINE_MANAGER_PORTS`) to connect to the manager
   started by the matching driver script.

The `cluster_driver_scripts/run_*.sh` files show the full end-to-end
pattern per workflow: submit the worker sbatch job, wait for it to reach
`RUNNING`, launch the manager with the condition's environment variables,
then cancel the worker job once the manager exits.

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

## Known limitations

- This repository packages the audit/replay system, workflows, and cluster
  orchestration; it does not include the specific experiment results,
  ablation-study tracking spreadsheet, or paper-figure-generation scripts.
  Reproducing the paper's exact published numbers requires re-running the
  conditions above and independently aggregating the output.
- Figures in the paper average over repeated runs are not available;
  single-run measurements were used throughout.
- Execution context reuse (`TASKVINE_WARM_POOL`) provides negligible
  benefit for the Minimap2 and GATK workflows, since their tasks shell out
  to compiled binaries rather than making Python function calls — this is
  a real, expected result of the mechanism's design (its module-signature
  matching operates over Python bytecode), not a bug.
- The elastic SLURM cluster used for the paper's evaluation is not
  included; `cluster_driver_scripts/` assumes SLURM (`sbatch`/`squeue`) is
  available and configured on your own cluster.
