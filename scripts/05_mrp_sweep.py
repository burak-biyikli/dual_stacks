#!/usr/bin/env python3
"""MRP confidence tuning sweep across workloads.

Runs a configurable matrix of MRP knob combinations on test-size workloads,
collects IPC and VP accuracy stats, and produces a ranked summary CSV.

Usage:
    python3 scripts/06_mrp_sweep.py --dry-run          # preview all configs
    python3 scripts/06_mrp_sweep.py --skip-build        # run with existing binary
    python3 scripts/06_mrp_sweep.py --jobs 12           # parallel execution
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import shlex
import subprocess
import sys
import threading
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass, asdict
from datetime import datetime
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
EXT_DIR = ROOT_DIR / "ext"
GEM5_DIR = EXT_DIR / "gem5"
GEM5_BIN = GEM5_DIR / "build/X86/gem5.opt"
PROFILE_ROOT = ROOT_DIR / "results/dr_tool_runs"
SWEEP_ROOT = ROOT_DIR / "results/mrp_sweeps"
CONFIG = ROOT_DIR / "configs/run_o3_benchmark_single.py"

# ---------------------------------------------------------------------------
# Sweep configuration matrix
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class MRPConfig:
    """One point in the MRP knob sweep space."""
    label: str
    log_max_confidence: int
    prediction_threshold: int
    allocation_confidence: int
    realloc_is_penalty: bool
    realloc_amount: int
    demotion_penalty: int

    @property
    def max_confidence(self) -> int:
        return (1 << self.log_max_confidence) - 1

    def cli_args(self) -> list[str]:
        args = [
            "--log-max-confidence", str(self.log_max_confidence),
            "--prediction-threshold", str(self.prediction_threshold),
            "--allocation-confidence", str(self.allocation_confidence),
            "--demotion-penalty", str(self.demotion_penalty),
        ]
        if self.realloc_is_penalty:
            args.extend(["--realloc-penalty", str(self.realloc_amount)])
        else:
            args.extend(["--realloc-reinit", str(self.realloc_amount)])
        return args


def build_sweep_configs() -> list[MRPConfig]:
    """Generate the sweep matrix.

    2-bit (max=3, threshold=3):
      - default:  alloc=1, demotion=1, realloc_penalty=1
      - targeted: alloc=2/dp=1, alloc=1/dp=2

    3-bit (max=7, threshold=7):
      - alloc   ∈ {4, 5, 6}
      - demotion ∈ {3, 4, 5}
      - realloc  ∈ {3, 4, 5}
    """
    configs: list[MRPConfig] = []

    # ---- 2-bit counter (max=3, threshold=3) ----
    # Current default
    configs.append(MRPConfig("default_2bit", 2, 3, 1, True, 1, 1))
    # Higher starting confidence (alloc=2): only needs 1 promotion to predict
    configs.append(MRPConfig("2bit_t3_a2_dp1_rp1", 2, 3, 2, True, 1, 1))
    # Harsher demotion (dp=2): single mispredict freezes from confidence 3
    configs.append(MRPConfig("2bit_t3_a1_dp2_rp1", 2, 3, 1, True, 1, 2))

    # ---- 3-bit counter (max=7, threshold=7) ----
    for alloc in [4, 5, 6]:
        for demotion in [3, 4, 5]:
            for realloc_amt in [3, 4, 5]:
                label = f"3bit_t7_a{alloc}_dp{demotion}_rp{realloc_amt}"
                configs.append(MRPConfig(label, 3, 7, alloc, True, realloc_amt, demotion))

    return configs


# ---------------------------------------------------------------------------
# Workload discovery
# ---------------------------------------------------------------------------

SIZE_MAP = {
    "test": {"parsec": "test", "gapbs": 10},
    "small": {"parsec": "simsmall", "gapbs": 15},
}


def split_profile_name(name: str):
    for suite in ("gapbs", "parsec"):
        prefix = f"{suite}_"
        if name.startswith(prefix):
            body = name[len(prefix):]
            benchmark, sep, size = body.rpartition("_")
            if sep and benchmark and size in SIZE_MAP:
                return suite, benchmark, size
    return None


def discover_workloads(profile_dir: Path, sizes: set[str], benchmarks: set[str]):
    """Find workloads with valid DynamoRIO profiles."""
    workloads = []
    for json_path in sorted(profile_dir.glob("*/parsed_data.json")):
        parsed = split_profile_name(json_path.parent.name)
        if not parsed:
            continue
        suite, benchmark, size = parsed
        if sizes and size not in sizes:
            continue
        if benchmarks and benchmark not in benchmarks and json_path.parent.name not in benchmarks:
            continue
        try:
            data = json.loads(json_path.read_text())
        except (OSError, json.JSONDecodeError):
            continue
        mem_ops = data.get("memory_ops", {})
        if mem_ops.get("total", 0) <= 0:
            continue
        workloads.append({
            "name": json_path.parent.name,
            "suite": suite,
            "benchmark": benchmark,
            "size": size,
        })
    return workloads


def workload_command(workload: dict, launcher: list[str], threads: int) -> tuple[list[str], Path]:
    """Build the command to run a workload."""
    suite = workload["suite"]
    benchmark = workload["benchmark"]
    size = workload["size"]

    if suite == "gapbs":
        binary = EXT_DIR / "gapbs" / benchmark
        cmd = launcher + [str(binary), "-g", str(SIZE_MAP[size]["gapbs"]), "-n", str(threads)]
        if benchmark == "sssp":
            cmd.extend(("-d", "2"))
        return cmd, ROOT_DIR

    parsecmgmt = EXT_DIR / "parsec-benchmark/bin/parsecmgmt"
    launcher_text = shlex.join(launcher)
    cmd = [str(parsecmgmt), "-a", "run", "-p", benchmark,
           "-c", "gcc", "-i", SIZE_MAP[size]["parsec"],
           "-n", str(threads), "-s", launcher_text]
    return cmd, EXT_DIR / "parsec-benchmark"


# ---------------------------------------------------------------------------
# Stats extraction
# ---------------------------------------------------------------------------

STAT_PATTERNS = {
    "ipc": re.compile(r"system\.cpu\.ipc\s+([\d.]+)"),
    "numCycles": re.compile(r"system\.cpu\.numCycles\s+(\d+)"),
    "VPsupported": re.compile(r"\.VPRenameSupported\s+(\d+)"),
    "VPpredicted": re.compile(r"\.VPRenamePredicted\s+(\d+)"),
    "VPcorrect": re.compile(r"\.VPCorrect\s+(\d+)"),
    "VPaccuracy": re.compile(r"\.VPaccuracy\s+([\d.]+)"),
    "VPcoverage": re.compile(r"\.VPcoverage\s+([\d.]+)"),
}


def parse_stats(stats_path: Path) -> dict[str, float]:
    result = {}
    if not stats_path.is_file():
        return result
    text = stats_path.read_text()
    for key, pattern in STAT_PATTERNS.items():
        m = pattern.search(text)
        if m:
            result[key] = float(m.group(1))
    return result


# ---------------------------------------------------------------------------
# Single-run worker (must be top-level for ProcessPoolExecutor)
# ---------------------------------------------------------------------------

def execute_one(run_info: dict, output_dir: str, max_insts: int,
                timeout: int, threads: int) -> dict:
    """Execute a single simulation and return the result dict."""
    outdir = Path(output_dir) / run_info["workload"] / run_info["config_label"]
    outdir.mkdir(parents=True, exist_ok=True)

    launcher = [
        str(GEM5_BIN),
        f"--outdir={outdir}",
        str(CONFIG),
        "--mr-mode", run_info["mr_mode"],
        "--max-insts", str(max_insts),
    ] + run_info["mrp_args"]

    command, cwd = workload_command(run_info, launcher, threads)

    with (outdir / "stdout.txt").open("w") as out, \
         (outdir / "stderr.txt").open("w") as err:
        try:
            proc = subprocess.run(command, cwd=cwd, stdout=out, stderr=err,
                                  timeout=timeout, check=False,
                                  env={**os.environ, "OMP_NUM_THREADS": str(threads)})
            status = "finished" if proc.returncode == 0 else "failed"
            returncode = proc.returncode
        except subprocess.TimeoutExpired:
            status = "timed_out"
            returncode = None

    stats = parse_stats(outdir / "stats.txt")
    return {
        **{k: v for k, v in run_info.items() if k != "mrp_args"},
        "status": status,
        "returncode": returncode,
        "outdir": str(outdir),
        **stats,
    }


# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

def apply_patches_and_build() -> None:
    commands = [
        ([str(SCRIPT_DIR / "00_patch_manager.sh"), "apply"], ROOT_DIR),
        (["scons", "build/X86/gem5.opt", f"-j{os.cpu_count() or 1}",
          "--linker=mold", "--ignore-style", "USE_CCACHE=1"], GEM5_DIR),
    ]
    for cmd, cwd in commands:
        print("[Build]", shlex.join(cmd))
        subprocess.run(cmd, cwd=cwd, check=True)


# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

def compute_summary(results: list[dict], output_dir: Path) -> None:
    """Compute per-config aggregates and write summary CSV."""
    by_config: dict[str, list[dict]] = {}
    for r in results:
        by_config.setdefault(r["config_label"], []).append(r)

    stock_ipc: dict[str, float] = {}
    for r in results:
        if r["config_label"] == "stock":
            ipc = r.get("ipc")
            if ipc and ipc > 0:
                stock_ipc[r["workload"]] = ipc

    summary_rows = []
    for label, runs in by_config.items():
        ipcs = [r["ipc"] for r in runs if r.get("ipc") and r["ipc"] > 0]
        accs = [r["VPaccuracy"] for r in runs if r.get("VPaccuracy") is not None and r["VPaccuracy"] >= 0]
        coverages = [r["VPcoverage"] for r in runs if r.get("VPcoverage") is not None and r["VPcoverage"] >= 0]

        speedups = []
        for r in runs:
            ipc = r.get("ipc")
            sipc = stock_ipc.get(r["workload"])
            if ipc and ipc > 0 and sipc and sipc > 0:
                speedups.append(ipc / sipc)

        row = {
            "config_label": label,
            "n_workloads": len(ipcs),
            "mean_ipc": sum(ipcs) / len(ipcs) if ipcs else 0,
            "geomean_ipc": math.exp(sum(math.log(x) for x in ipcs) / len(ipcs)) if ipcs else 0,
            "mean_speedup": sum(speedups) / len(speedups) if speedups else 0,
            "geomean_speedup": math.exp(sum(math.log(x) for x in speedups) / len(speedups)) if speedups else 0,
            "mean_accuracy": sum(accs) / len(accs) if accs else 0,
            "mean_coverage": sum(coverages) / len(coverages) if coverages else 0,
            "n_finished": sum(1 for r in runs if r.get("status") == "finished"),
            "n_failed": sum(1 for r in runs if r.get("status") != "finished"),
        }
        summary_rows.append(row)

    summary_rows.sort(key=lambda r: r["geomean_speedup"], reverse=True)

    summary_path = output_dir / "sweep_summary.csv"
    fields = list(summary_rows[0].keys()) if summary_rows else []
    with summary_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary_rows)

    print(f"\n{'='*94}")
    print(f"MRP SWEEP SUMMARY — Top configurations by geomean speedup vs stock")
    print(f"{'='*94}")
    print(f"{'Config':<35} {'GeoSpeedup':>10} {'MeanSpeedup':>11} {'GeoIPC':>8} {'MeanIPC':>8} {'Accuracy':>8} {'Coverage':>8}")
    print(f"{'-'*35} {'-'*10} {'-'*11} {'-'*8} {'-'*8} {'-'*8} {'-'*8}")
    for row in summary_rows[:25]:
        print(f"{row['config_label']:<35} {row['geomean_speedup']:>10.4f} {row['mean_speedup']:>11.4f} "
              f"{row['geomean_ipc']:>8.4f} {row['mean_ipc']:>8.4f} "
              f"{row['mean_accuracy']:>8.4f} {row['mean_coverage']:>8.4f}")

    print(f"\nFull summary: {summary_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def parse_args():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile-dir", default="2026-07-02_14-10-54",
                        help="timestamp directory under results/dr_tool_runs")
    parser.add_argument("--sizes", default="test", help="comma-separated sizes (default: test)")
    parser.add_argument("--benchmarks", default="", help="filter to specific benchmarks (comma-separated)")
    parser.add_argument("--max-insts", type=int, default=1_000_000)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--threads", type=int, default=1,
                        help="OMP_NUM_THREADS per simulation (default: 1)")
    parser.add_argument("--jobs", "-j", type=int, default=1,
                        help="number of simulations to run in parallel (default: 1)")
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--output-dir", help="output directory (default: timestamped)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    # Resolve profile dir
    profile_path = Path(args.profile_dir)
    if not profile_path.is_absolute():
        profile_path = PROFILE_ROOT / profile_path
    if not profile_path.is_dir():
        print(f"Profile directory not found: {profile_path}", file=sys.stderr)
        return 1

    sizes = {s.strip() for s in args.sizes.split(",") if s.strip()}
    benchmarks = {b.strip() for b in args.benchmarks.split(",") if b.strip()}
    workloads = discover_workloads(profile_path, sizes, benchmarks)
    if not workloads:
        print("No workloads found!", file=sys.stderr)
        return 1

    configs = build_sweep_configs()
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    output_dir = Path(args.output_dir) if args.output_dir else SWEEP_ROOT / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)

    total = len(workloads) + len(configs) * len(workloads)
    est_batches = math.ceil(total / max(args.jobs, 1))
    print(f"Sweep: {len(configs)} MRP configs + stock baseline × {len(workloads)} workloads = {total} runs")
    print(f"Parallelism: {args.jobs} jobs → ~{est_batches} batches")
    print(f"Output: {output_dir}")

    # Build run plan
    plan: list[dict] = []

    for wl in workloads:
        plan.append({
            "workload": wl["name"],
            "config_label": "stock",
            "mr_mode": "off",
            "mrp_args": [],
            **wl,
        })

    for cfg in configs:
        for wl in workloads:
            plan.append({
                "workload": wl["name"],
                "config_label": cfg.label,
                "mr_mode": "full",
                "mrp_args": cfg.cli_args(),
                **wl,
            })

    plan_path = output_dir / "sweep_plan.json"
    plan_path.write_text(json.dumps({
        "created_at": timestamp,
        "profile_dir": str(profile_path),
        "n_configs": len(configs),
        "n_workloads": len(workloads),
        "n_total_runs": len(plan),
        "jobs": args.jobs,
        "configs": [asdict(c) for c in configs],
        "workloads": workloads,
    }, indent=2) + "\n")

    if args.dry_run:
        print(f"\n[DRY RUN] Would execute {len(plan)} simulations with {args.jobs} parallel jobs.")
        print(f"Plan saved to: {plan_path}")
        for cfg in configs:
            print(f"  {cfg.label}: {' '.join(cfg.cli_args())}")
        return 0

    # Build gem5
    if args.skip_build:
        if not GEM5_BIN.is_file():
            print(f"gem5 binary not found: {GEM5_BIN}", file=sys.stderr)
            return 1
        print(f"Reusing existing gem5 binary: {GEM5_BIN}")
    else:
        try:
            apply_patches_and_build()
        except subprocess.CalledProcessError as e:
            print(f"Build failed: {e}", file=sys.stderr)
            return 1

    # Execute with parallelism
    results: list[dict] = []
    failures = 0
    completed_count = 0
    print_lock = threading.Lock()

    output_dir_str = str(output_dir)

    if args.jobs <= 1:
        # Sequential path (simpler output)
        for i, run_info in enumerate(plan, 1):
            result = execute_one(run_info, output_dir_str, args.max_insts,
                                 args.timeout, args.threads)
            results.append(result)
            status_icon = "✓" if result["status"] == "finished" else "✗"
            ipc_str = f"IPC={result.get('ipc', 0):.4f}" if "ipc" in result else "no stats"
            acc_str = f"Acc={result.get('VPaccuracy', 0):.3f}" if "VPaccuracy" in result else ""
            print(f"  [{i}/{len(plan)}] {status_icon} {result['workload']}/{result['config_label']}: {ipc_str} {acc_str}")
            if result["status"] != "finished":
                failures += 1
    else:
        # Parallel path
        print(f"\nLaunching {len(plan)} simulations across {args.jobs} workers...")
        with ProcessPoolExecutor(max_workers=args.jobs) as executor:
            future_to_info = {}
            for run_info in plan:
                future = executor.submit(
                    execute_one, run_info, output_dir_str,
                    args.max_insts, args.timeout, args.threads,
                )
                future_to_info[future] = run_info

            for future in as_completed(future_to_info):
                completed_count += 1
                result = future.result()
                results.append(result)

                status_icon = "✓" if result["status"] == "finished" else "✗"
                ipc_str = f"IPC={result.get('ipc', 0):.4f}" if "ipc" in result else "no stats"
                acc_str = f"Acc={result.get('VPaccuracy', 0):.3f}" if "VPaccuracy" in result else ""

                with print_lock:
                    print(f"  [{completed_count}/{len(plan)}] {status_icon} {result['workload']}/{result['config_label']}: {ipc_str} {acc_str}")

                if result["status"] != "finished":
                    failures += 1

    # Save raw results
    raw_path = output_dir / "sweep_results.csv"
    fields = ["workload", "config_label", "status", "ipc", "numCycles",
              "VPsupported", "VPpredicted", "VPcorrect", "VPaccuracy", "VPcoverage",
              "mr_mode", "suite", "benchmark", "size", "outdir"]
    with raw_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(results)

    print(f"\nRaw results: {raw_path}")

    # Compute and display summary
    compute_summary(results, output_dir)

    print(f"\nCompleted {len(plan)} simulations; {failures} failures.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
