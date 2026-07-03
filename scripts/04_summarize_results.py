#!/usr/bin/env python3

import os
import sys
import json
import argparse
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
RESULTS_DIR = ROOT_DIR / "results/dr_tool_runs"

def calculate_percentage(part, whole):
    if not whole or whole == 0:
        return "N/A"
    return f"{(part / whole) * 100:.2f}%"

def summarize_run(run_dir):
    print(f"Summarizing data from: {run_dir}")
    print("-" * 155)
    print(f"{'Benchmark(ProcessName)':<40} {'Size':<10} {'LD%':<8} {'ST%':<8} {'PUSH%':<8} {'POP%':<8} {'Private%':<10} {'StrictIPC%':<12} {'PossIPC(H)%':<13} {'PossIPC(NH)%':<14} {'PUSH/ST':<10} {'POP/LD':<10}")
    print("-" * 155)
    
    # Sort subdirectories to have a consistent order
    bench_dirs = sorted([d for d in run_dir.iterdir() if d.is_dir()])
    
    for bench_dir in bench_dirs:
        json_path = bench_dir / "parsed_data.json"
        if not json_path.exists():
            continue
            
        with open(json_path, "r") as f:
            data = json.load(f)
            
        # Parse benchmark name and size
        # Expected format: gapbs_bfs_test or parsec_blackscholes_simsmall
        name_parts = bench_dir.name.split('_')
        if len(name_parts) >= 3:
            size = name_parts[-1]
            name = "_".join(name_parts[:-1])
        else:
            size = "unknown"
            name = bench_dir.name
            
        app_name = data.get("app_name", "unknown")
        run_status = data.get("run_status", "unknown")
        status_char = "*" if run_status != "finished" else ""
        bench_display = f"{name}({app_name}{status_char})"
            
        mem = data.get("memory_ops", {})
        stack = data.get("stack_ops", {})
        
        mem_tot = mem.get("total", 0)
        ld = mem.get("loads", 0)
        st = mem.get("stores", 0)
        push = mem.get("pushes", 0)
        pop = mem.get("pops", 0)
        
        stack_tot = stack.get("total", 0)
        priv = stack.get("provably_private", 0)
        strict = stack.get("strictly_ipc", 0)
        poss_nh = stack.get("possibly_ipc_non_hist", 0)
        poss_h = stack.get("possibly_ipc_hist", 0)
        
        if mem_tot == 0:
            # Benchmark likely failed
            print(f"{bench_display:<40} {size:<10} {'Failed':<8} {'-':<8} {'-':<8} {'-':<8} {'-':<10} {'-':<12} {'-':<13} {'-':<14} {'-':<10} {'-':<10}")
            continue
            
        # Mathematical Invariants
        try:
            assert stack_tot == push + pop, f"[{name}] Stack total ({stack_tot}) != pushes ({push}) + pops ({pop})"
            assert stack_tot == priv + strict + poss_nh, f"[{name}] Stack total ({stack_tot}) != priv ({priv}) + strict ({strict}) + poss_nh ({poss_nh})"
            assert poss_nh >= poss_h, f"[{name}] Non-History IPC ({poss_nh}) < History IPC ({poss_h})"

            
            unique_pcs = data.get("unique_pcs", 0)
            unique_ipc_pcs = data.get("unique_ipc_pcs", 0)
            if unique_pcs > 0 or unique_ipc_pcs > 0:
                assert unique_pcs >= unique_ipc_pcs, f"[{name}] Unique PCs ({unique_pcs}) < IPC PCs ({unique_ipc_pcs})"
                
            histo = data.get("histogram", {})
            histo_sum = sum(histo.values())
            assert histo_sum <= push, f"[{name}] Histogram sum ({histo_sum}) > total pushes ({push})"
            # Note: We will use <= for safety because of extreme edge cases (like crash) instead of == which should always be true as
            # analyzer_cleanup is called at exit, and so all pushes SHOULD be popped or forcefully evicted by the max-clock approximation.
             
        except AssertionError as e:
            print(f"INVARIANT FAILED: {e}")
            continue
            
        ld_pct = calculate_percentage(ld, mem_tot)
        st_pct = calculate_percentage(st, mem_tot)
        push_pct = calculate_percentage(push, mem_tot)
        pop_pct = calculate_percentage(pop, mem_tot)
        
        priv_pct = calculate_percentage(priv, stack_tot)
        strict_pct = calculate_percentage(strict, stack_tot)
        poss_nh_pct = calculate_percentage(poss_nh, stack_tot)
        poss_h_pct = calculate_percentage(poss_h, stack_tot)
        
        push_st = calculate_percentage(push, st)
        pop_ld = calculate_percentage(pop, ld)
        
        print(f"{bench_display:<40} {size:<10} {ld_pct:<8} {st_pct:<8} {push_pct:<8} {pop_pct:<8} {priv_pct:<10} {strict_pct:<12} {poss_h_pct:<13} {poss_nh_pct:<14} {push_st:<10} {pop_ld:<10}")

def main():
    parser = argparse.ArgumentParser(description="Summarize profiling run results")
    parser.add_argument("--dir", type=str, help="Specific timestamp directory to summarize. Defaults to latest.")
    args = parser.parse_args()
    
    if args.dir:
        target_dir = Path(args.dir)
        if not target_dir.is_absolute():
            target_dir = RESULTS_DIR / args.dir
    else:
        # Find latest
        runs = [d for d in RESULTS_DIR.iterdir() if d.is_dir()]
        if not runs:
            print(f"No run directories found in {RESULTS_DIR}")
            sys.exit(1)
        # Sort by name (which is timestamp) and get last
        target_dir = sorted(runs)[-1]
        
    if not target_dir.exists() or not target_dir.is_dir():
        print(f"Error: Directory {target_dir} not found.")
        sys.exit(1)
        
    summarize_run(target_dir)

if __name__ == "__main__":
    main()
