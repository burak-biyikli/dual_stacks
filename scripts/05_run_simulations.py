#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

# Setup paths
SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
EXT_DIR = ROOT_DIR / "ext"
GEM5_DIR = EXT_DIR / "gem5"

def run_cmd(cmd, cwd=ROOT_DIR):
    """Helper to run a shell command and check for errors."""
    print(f"\n[Running] {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0:
        print(f"[Error] Command failed with exit code {result.returncode}")
        sys.exit(result.returncode)

def main():
    print("=== 1. Ensuring Submodule Patches are Applied ===")
    run_cmd([str(SCRIPT_DIR / "00_patch_manager.sh"), "apply"])

    print("=== 2. Ensuring gem5 is Up-to-Date ===")
    # Utilizing ccache to make this rebuild nearly instantaneous if nothing changed
    run_cmd([
        "scons",
        "build/X86/gem5.opt",
        "-j" + str(os.cpu_count()),
        "--linker=mold",
        "--ignore-style",
        "USE_CCACHE=1"
    ], cwd=GEM5_DIR)

    print("=== 3. Running Simulations ===")
    # Example logic for iterating over generated configs
    # configs_dir = ROOT_DIR / "configs" / "generated"
    # for config_file in configs_dir.glob("*.py"):
    #     run_name = config_file.stem
    #     out_dir = ROOT_DIR / "results" / "gem5_sim" / run_name
    #     
    #     print(f"--> Simulating {run_name} ...")
    #     run_cmd([
    #         str(GEM5_DIR / "build/X86/gem5.opt"),
    #         f"--outdir={out_dir}",
    #         str(config_file)
    #     ])
    
    print("Simulations would run here! Pipeline is ready.")

if __name__ == "__main__":
    main()
