#!/usr/bin/env python3

import os
import sys
import argparse
import subprocess
import json
import re
from datetime import datetime
from pathlib import Path

# Paths
ROOT_DIR = Path(__file__).resolve().parent.parent
EXT_DIR = ROOT_DIR / "ext"
DRRUN = EXT_DIR / "dynamorio/build/bin64/drrun"
CLIENT = ROOT_DIR / "tools/trace_analyzer/build/libanalyzer.so"
RESULTS_DIR = ROOT_DIR / "results/dr_tool_runs"

PARSEC_BENCHMARKS = [
    "blackscholes", "bodytrack", "canneal", "dedup", "facesim",
    "ferret", "fluidanimate", "freqmine", "streamcluster", "swaptions", "vips", "x264"
]

GAPBS_BENCHMARKS = ["bfs", "bc", "cc", "pr", "sssp", "tc"]

SIZE_MAP = {
    "test": {"parsec": "test", "gapbs": 10},
    "small": {"parsec": "simsmall", "gapbs": 15},
    "medium": {"parsec": "simmedium", "gapbs": 18},
    "large": {"parsec": "simlarge", "gapbs": 20},
    "native": {"parsec": "native", "gapbs": 22}
}

def get_sys_info():
    info = []
    try:
        uname = subprocess.check_output(["uname", "-a"], text=True).strip()
        info.append(f"OS: {uname}")
    except: pass
    
    try:
        lscpu = subprocess.check_output(["lscpu"], text=True)
        for line in lscpu.split('\n'):
            if "Model name:" in line:
                info.append(f"CPU: {line.split(':', 1)[1].strip()}")
                break
    except: pass
    
    try:
        git_hash = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True, cwd=ROOT_DIR).strip()
        git_dirty = subprocess.run(["git", "diff", "--quiet"], cwd=ROOT_DIR).returncode != 0
        info.append(f"Git Hash: {git_hash}{' (dirty)' if git_dirty else ''}")
    except: pass

    return "\n".join(info)

def parse_analyzer_output(stderr_text, status):
    blocks = stderr_text.split("=== Stack Privacy Analysis ===")
    if len(blocks) <= 1:
        return {"memory_ops": {}, "stack_ops": {}, "pcs": {}, "histogram": {}}
        
    best_data = None
    max_mem_ops = -1
    
    for block in blocks[1:]:
        data = {
            "app_name": "unknown",
            "run_status": "unknown",
            "memory_ops": {},
            "stack_ops": {},
            "pcs": {},
            "histogram": {}
        }
        
        if m := re.search(r"\[APP:\s*([^\]]+)\]", block):
            data["app_name"] = m.group(1).strip()

        data["run_status"] = status
            
        patterns = {
            "memory_ops": {
                "total": r"Total Memory Operations:\s*(\d+)",
                "loads": r"Loads:\s*(\d+)",
                "stores": r"Stores:\s*(\d+)",
                "pushes": r"Pushes:\s*(\d+)",
                "pops": r"Pops:\s*(\d+)"
            },
            "stack_ops": {
                "total": r"Stack Operations \(PUSH\+POP\):\s*(\d+)",
                "provably_private": r"Provably Private:\s*(\d+)",
                "strictly_ipc": r"Strictly IPC \(Instance Level\):\s*(\d+)",
                "possibly_ipc_non_hist": r"Possibly IPC \(Non-History\):\s*(\d+)",
                "possibly_ipc_hist": r"Possibly IPC \(History-Based, \d+ buckets\):\s*(\d+)"
            },
            "pcs": {
                "unique": r"Unique Stack PCs:\s*(\d+)",
                "with_ipc": r"Unique Stack PCs with IPC:\s*(\d+)"
            }
        }
        
        for category, fields in patterns.items():
            for field, pat in fields.items():
                if m := re.search(pat, block):
                    data[category][field] = int(m.group(1))
                    
        # Parse histogram
        in_histogram = False
        for line in block.split('\n'):
            if "=== Stack Lifetime Histogram" in line:
                in_histogram = True
                continue
            
            if in_histogram:
                if m := re.search(r"^\s*\[([^\]]+)\]:\s*(\d+)", line):
                    data["histogram"][m.group(1).strip()] = int(m.group(2))
                elif line.strip() == "" or line.startswith("===") or line.startswith("[PARSEC]"):
                    if len(data["histogram"]) > 0:
                        break # End of histogram
                        
        mem_ops = data["memory_ops"].get("total", 0)
        if mem_ops > max_mem_ops:
            max_mem_ops = mem_ops
            best_data = data
            
    return best_data if best_data else {"memory_ops": {}, "stack_ops": {}, "pcs": {}, "histogram": {}}

def run_benchmark(bench_name, cmd, cwd, out_dir, timeout_secs):
    print(f"Running {bench_name}...")
    bench_dir = out_dir / bench_name
    bench_dir.mkdir(parents=True, exist_ok=True)
    
    stdout_path = bench_dir / "stdout.txt"
    stderr_path = bench_dir / "stderr.txt"
    json_path = bench_dir / "parsed_data.json"
    
    import os
    import signal
    import time
    
    if timeout_secs > 0:
        cmd = cmd # Remove timeout wrapper since we handle it in Python
        
    try:
        with open(stdout_path, "w") as out_f, open(stderr_path, "w") as err_f:
            process = subprocess.Popen(cmd, shell=True, cwd=cwd, stdout=out_f, stderr=err_f, preexec_fn=os.setsid)
            status = "finished"
            
            if timeout_secs > 0:
                try:
                    process.wait(timeout=timeout_secs)
                except subprocess.TimeoutExpired:
                    status = "timed_out"
                    fifo_path = f"/tmp/dr_analyzer_fifo_{os.getpid()}"
                    try:
                        # Open non-blocking in case no reader exists, to avoid hanging python
                        fd = os.open(fifo_path, os.O_WRONLY | os.O_NONBLOCK)
                        os.write(fd, b'T' * 100)
                        os.close(fd)
                    except OSError:
                        pass # No reader or pipe error
                        
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(os.getpgid(process.pid), signal.SIGKILL)
                        process.wait()
                    time.sleep(3)  # Allow orphaned drrun to finish dumping to stderr
            else:
                process.wait()
                
            if status != "timed_out" and process.returncode != 0:
                status = "failed"
            
            # Append status to stdout
            out_f.write(f"\n[Trace Analyzer Script] Run Status: {status}\n")
            
        with open(stdout_path, "r") as out_f, open(stderr_path, "r") as err_f:
            parsed = parse_analyzer_output(out_f.read() + "\n" + err_f.read(), status)
            
        with open(json_path, "w") as j_f:
            json.dump(parsed, j_f, indent=2)
            
        if status == "timed_out":
            print(f"  -> Warn: Benchmark timed out after {timeout_secs} seconds. Extracted {len(parsed.get('histogram', {}))} histogram buckets.")
        elif status == "failed":
            print(f"  -> Failed: Benchmark exited with code {process.returncode}. Extracted {len(parsed.get('histogram', {}))} histogram buckets.")
        else:
            print(f"  -> Finished! Extracted {len(parsed.get('histogram', {}))} histogram buckets.")
            
    except Exception as e:
        print(f"  -> Failed with exception: {e}")

def main():
    import multiprocessing
    parser = argparse.ArgumentParser(description="Profile workloads with DynamoRIO trace analyzer")
    parser.add_argument("--size", choices=list(SIZE_MAP.keys()) + ["all"], default="test", help="Dataset size to run")
    parser.add_argument("--threads", type=int, default=4, help="Number of threads to use for benchmarks")
    parser.add_argument("--timeout", type=int, default=1800, help="Timeout in seconds for each run (default: 1800, 0 to disable)")
    args = parser.parse_args()
    
    sizes_to_run = list(SIZE_MAP.keys()) if args.size == "all" else [args.size]
    
    print("Ensuring trace analyzer is up to date...")
    try:
        subprocess.run(["make"], cwd=ROOT_DIR / "tools/trace_analyzer/build", check=True)
    except subprocess.CalledProcessError:
        print("Error: Failed to build trace_analyzer.")
        sys.exit(1)
        
    if not DRRUN.exists() or not CLIENT.exists():
        print("Error: DynamoRIO or trace_analyzer client not found. Run 01_setup.sh first.")
        sys.exit(1)
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    out_dir = RESULTS_DIR / timestamp
    out_dir.mkdir(parents=True, exist_ok=True)
    
    with open(out_dir / "sys_info.txt", "w") as f:
        f.write(get_sys_info())
        
    fifo_path = f"/tmp/dr_analyzer_fifo_{os.getpid()}"
    try:
        os.mkfifo(fifo_path)
    except OSError:
        pass
        
    dr_cmd = f"{DRRUN} -c {CLIENT} -fifo_path {fifo_path} --"
    
    # Export OMP_NUM_THREADS for GAPBS
    os.environ["OMP_NUM_THREADS"] = str(args.threads)
    
    for size in sizes_to_run:
        gapbs_scale = SIZE_MAP[size]["gapbs"]
        parsec_size = SIZE_MAP[size]["parsec"]
        
        # Run GAPBS
        for bench in GAPBS_BENCHMARKS:
            bin_path = EXT_DIR / "gapbs" / bench
            if not bin_path.exists():
                print(f"Warning: {bin_path} not found. Skipping.")
                continue
            
            extra_args = "-d 2" if bench == "sssp" else ""
            cmd = f"{dr_cmd} {bin_path} -g {gapbs_scale} -n 1 {extra_args}"
            run_benchmark(f"gapbs_{bench}_{size}", cmd, ROOT_DIR, out_dir, args.timeout)
            
        # Run PARSEC
        for bench in PARSEC_BENCHMARKS:
            # We assume PARSEC is built. parsecmgmt handles running.
            cmd = f"./bin/parsecmgmt -a run -p {bench} -c gcc -i {parsec_size} -n {args.threads} -s '{dr_cmd}'"
            run_benchmark(f"parsec_{bench}_{size}", cmd, EXT_DIR / "parsec-benchmark", out_dir, args.timeout)
            
    print(f"\nAll runs complete! Results saved in {out_dir}")
    try:
        os.unlink(fifo_path)
    except OSError:
        pass

if __name__ == "__main__":
    main()
