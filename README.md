# Microarchitectural Research Environment

This is the source code for the blog post at burakbiyikli.com which details the dual stack idea. The implementation here utilizes the DynamoRIO tracer and gem5 simulator. The workflow determines equivlent structure size changes per workload via DynamoRIO, which are then modeled in gem5 as a proxy. This repo has mainly been tested on the smaller PARSEC and GAPBS workloads due to time limits of the author.

## Directory Organization

* **`ext/`**: Upstream dependencies managed as git submodules (`gem5`, `dynamorio`, `gapbs`, `parsec-benchmark`).
* **`configs/`**: Our custom gem5 python system configuration scripts (where we apply our hardware modifications) and the unified experiments specification `experiments.json` defining parameter sweep matrices.
* **`patches/`**: Diff files containing our custom C++ modifications to the upstream `ext/` submodules.
* **`results/`**: Output data organized by pipeline stage:
  * `dr_tool_runs/` - Timestamped directories containing DynamoRIO profiling output (`parsed_data.json`, stdout/stderr logs).
  * `gem5_sim_runs/` - Timestamped directories containing gem5 simulation results (`stats.txt`, `run_manifest.json`, `consolidated_results.csv`, `sweep_summary.csv`).
  * `tmp/` - Intermediate/test outputs (symlinks to m5out directories, temporary files).
* **`scripts/`**: Automation scripts for executing the end-to-end pipeline.
* **`tools/`**: Custom tools and test programs. Notably `trace_analyzer`, which is our DynamoRIO C++ client, and `gem5_tests/` for validation.
* **`/tmp`**: Temp files used by scripts
  * `/tmp/gem5_dual_stacks_tests/` - Simulation files used by `gem5_tests/`
  * `/tmp/dr_analyzer_fifo_*` - Temp files used by `trace_analyzer`

## Scripts Workflow

The pipeline is split into distinct, sequentially executable scripts for ease of debugging and development:

* **`00_patch_manager.sh`**: A helper script to `apply` existing patches to the `ext/` submodules or `update` them based on your local uncommitted changes.
* **`01_setup.sh`**: Initializes the environment by pulling submodules, installing system dependencies, compiling DynamoRIO, GAPBS, our C++ analyzer, and building gem5.
* **`02_test.sh`**: Health check script that executes "Hello World" sanity checks on the native machine, DynamoRIO, GAPBS, PARSEC, and gem5 to ensure everything is functioning.
* **`03_profile_workloads.py`**: Executes the PARSEC and GAPBS benchmark suites under the DynamoRIO `trace_analyzer` client and extracts metrics to `results/dr_tool_runs/`.
* **`04_summarize_results.py`**: Parses the raw JSON metrics from `03_profile_workloads.py` and outputs tabulated distributions of stack privacy and IPC.
* **`05_run_simulations.py`**: Manifest-driven parallel runner for gem5 macrobenchmark studies. Uses sweep definitions in `configs/experiments.json` to automatically compute Cartesian cross-products, parallel-dispatches simulations using `ProcessPoolExecutor`, and writes the execution database to `results/gem5_sim_runs/`.
* **`06_extract_data.py`**: Pulls simulation results matching the manifest, parses `stats.txt`, produces `consolidated_results.csv` with all flat metric columns, computes aggregate speedups vs. baseline, and generates `sweep_summary.csv`.

> [!TIP]
> **Timeout for Long-Running Profiling**
> Because heavy C++ workloads (e.g., `bodytrack`) can take a very long time to execute under dense DynamoRIO tracing, `03_profile_workloads.py` supports a `--timeout <seconds>` flag. Using this flag safely wraps the execution in a signal-terminating boundary. If the timeout is hit, the custom DynamoRIO client intercepts the `SIGTERM` and executes a clean termination, gracefully flushing all hash tables and outputting whatever data was successfully collected before the timeout expired.

> [!NOTE]
> **Synthetic (`-g`) vs. Real-World Datasets**
> Using the `-g` flag generates a synthetic Kronecker graph on the fly, which is excellent for lightweight functional testing and avoids heavy disk/RAM usage. However, synthetic graphs are mathematically "smooth" and lack the irregular community clustering and extreme degree skews of real-world networks. This can lead to artificially high cache efficiency (low Last Level Cache misses) compared to real datasets.
>
> For robust architectural profiling, it is recommended to download real-world GAP datasets (e.g., `GAP-twitter`, `GAP-web`, `GAP-road`) in Matrix Market (`.mtx`) format from the **SuiteSparse Matrix Collection**. These can be converted via the GAPBS `./converter` tool to the `.sg` format. 
> * **Size Note:** While generating these graphs from raw web crawls takes ~275GB of disk space and 64GB of RAM, the compiled `.sg` files for the core SuiteSparse graphs are only **~6GB to 10GB each** and can be run comfortably on systems with 32GB of RAM.

> [!NOTE]
> **Known Performance limitation** It would be good to investigate the performance of the DynamoRIO `trace_analyzer` tool more closely. Some benchmarks (like `parsec_bodytrack`) experience extreme geometric slowdowns under dense instrumentation. There may be some benifit to iterating on the tracer for better runtime performance.
