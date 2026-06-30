# Microarchitectural Research Environment

This repository provides an automated, end-to-end pipeline for performing microarchitectural research using DynamoRIO tracing and the gem5 simulator. It is designed to profile PARSEC and GAPBS workloads natively to extract memory footprint / communication metrics, and then pipe those metrics into gem5 configuration scripts (e.g. to scale LSU structures).

## Directory Organization

* **`ext/`**: Upstream dependencies managed as git submodules (`gem5`, `dynamorio`, `gapbs`, `parsec-benchmark`).
* **`configs/`**: Our custom gem5 python system configuration scripts (where we apply our hardware modifications).
* **`patches/`**: Diff files containing our custom C++ modifications to the upstream `ext/` submodules.
* **`results/`**: Output data, containing trace summaries, generated configurations, and gem5 `stats.txt` runs.
* **`scripts/`**: Automation scripts for executing the end-to-end pipeline.
* **`tools/`**: Custom tools, particularly `trace_analyzer`, which is our DynamoRIO C++ client.

> [!NOTE]
> **Synthetic (`-g`) vs. Real-World Datasets**
> Using the `-g` flag generates a synthetic Kronecker graph on the fly, which is excellent for lightweight functional testing and avoids heavy disk/RAM usage. However, synthetic graphs are mathematically "smooth" and lack the irregular community clustering and extreme degree skews of real-world networks. This can lead to artificially high cache efficiency (low Last Level Cache misses) compared to real datasets.
>
> For robust architectural profiling, it is recommended to download real-world GAP datasets (e.g., `GAP-twitter`, `GAP-web`, `GAP-road`) in Matrix Market (`.mtx`) format from the **SuiteSparse Matrix Collection**. These can be converted via the GAPBS `./converter` tool to the `.sg` format. 
> * **Size Note:** While generating these graphs from raw web crawls takes ~275GB of disk space and 64GB of RAM, the compiled `.sg` files for the core SuiteSparse graphs are only **~6GB to 10GB each** and can be run comfortably on systems with 32GB of RAM.

- [ ] **TODO:** Find and add support for pulling a robust, real-world graph from the SuiteSparse Matrix Collection for final architectural evaluations.
- [ ] **TODO:** Investigate the performance of the DynamoRIO `trace_analyzer` tool more closely. Some benchmarks (like `parsec_bodytrack`) experience extreme geometric slowdowns under dense instrumentation.

## Scripts Workflow

The pipeline is split into distinct, sequentially executable scripts for ease of debugging and development:

* **`00_patch_manager.sh`**: A helper script to `apply` existing patches to the `ext/` submodules or `update` them based on your local uncommitted changes.
* **`01_setup.sh`**: Initializes the environment by pulling submodules, installing system dependencies, compiling DynamoRIO, GAPBS, our C++ analyzer, and building gem5.
* **`02_test.sh`**: Health check script that executes "Hello World" sanity checks on the native machine, DynamoRIO, GAPBS, PARSEC, and gem5 to ensure everything is functioning.
* **`03_profile_workloads.py`**: Executes the PARSEC and GAPBS benchmark suites under the DynamoRIO `trace_analyzer` client and extracts metrics to `results/dr_tool_runs/`.
* **`04_summarize_results.py`**: Parses the raw JSON metrics from `03_profile_workloads.py` and outputs tabulated distributions of stack privacy and IPC.

> [!TIP]
> **Timeout for Long-Running Profiling**
> Because heavy C++ workloads (e.g., `bodytrack`) can take a very long time to execute under dense DynamoRIO tracing, `03_profile_workloads.py` supports a `--timeout <seconds>` flag. Using this flag safely wraps the execution in a signal-terminating boundary. If the timeout is hit, the custom DynamoRIO client intercepts the `SIGTERM` and executes a clean termination, gracefully flushing all hash tables and outputting whatever data was successfully collected before the timeout expired.

*(Future scripts to be added: `05_run_simulations.py`, `06_extract_data.py`)*
