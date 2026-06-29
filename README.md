# Microarchitectural Research Environment

This repository provides an automated, end-to-end pipeline for performing microarchitectural research using DynamoRIO tracing and the gem5 simulator. It is designed to profile PARSEC and GAPBS workloads natively to extract memory footprint / communication metrics, and then pipe those metrics into gem5 configuration scripts (e.g. to scale LSU structures).

## Directory Organization

* **`ext/`**: Upstream dependencies managed as git submodules (`gem5`, `dynamorio`, `gapbs`, `parsec-benchmark`).
* **`configs/`**: Our custom gem5 python system configuration scripts (where we apply our hardware modifications).
* **`patches/`**: Diff files containing our custom C++ modifications to the upstream `ext/` submodules.
* **`results/`**: Output data, containing trace summaries, generated configurations, and gem5 `stats.txt` runs.
* **`scripts/`**: Automation scripts for executing the end-to-end pipeline.
* **`tools/`**: Custom tools, particularly `trace_analyzer`, which is our DynamoRIO C++ client.

## Scripts Workflow

The pipeline is split into distinct, sequentially executable scripts for ease of debugging and development:

* **`00_patch_manager.sh`**: A helper script to `apply` existing patches to the `ext/` submodules or `update` them based on your local uncommitted changes.
* **`01_setup.sh`**: Initializes the environment by pulling submodules, installing system dependencies, compiling DynamoRIO, GAPBS, our C++ analyzer, and building gem5.
* **`02_test.sh`**: Health check script that executes "Hello World" sanity checks on the native machine, DynamoRIO, GAPBS, PARSEC, and gem5 to ensure everything is functioning perfectly.

*(Future scripts to be added: `03_profile_workloads.py`, `04_run_simulations.py`, `05_extract_data.py`)*