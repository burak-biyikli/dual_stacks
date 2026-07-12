#!/usr/bin/env python3
"""
Extract key statistics from gem5 simulation runs using run_manifest.json.
Outputs a consolidated CSV suitable for Google Sheets analysis.
"""

import argparse
import csv
import json
import sys
import math
from pathlib import Path

# Module-level paths
SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
SIM_ROOT = ROOT_DIR / "results/gem5_sim_runs"

# -------------------------------------------------------------------------
# Extensibility Dictionary: Add new stats to extract here.
# Format -> "gem5_stat_string": "CSV Column Header"
# -------------------------------------------------------------------------
TARGET_STATS = {
    "simSeconds": "Runtime (s)",
    "simInsts": "Insts Simulated",
    "system.cpu.ipc": "IPC",
    "system.cpu.cpi": "CPI",

    # LSU & Memory Pipeline
    "system.cpu.lsq0.addedLoadsAndStores": "LSU Entries Allocated",
    "system.cpu.commitStats0.numLoadInsts": "Committed Loads",
    "system.cpu.commitStats0.numStoreInsts": "Committed Stores",

    # Predictor / Memory Renaming Unit
    "system.cpu.valuePred.VPRenameSupported": "VP Supported",
    "system.cpu.valuePred.VPRenamePredicted": "VP Predicted",
    "system.cpu.valuePred.VPCorrect": "VP Correct",
    "system.cpu.valuePred.VPMispredict": "VP Mispredict",
    "system.cpu.valuePred.VPMissedOpportunity": "VP Missed Opportunity",
    "system.cpu.valuePred.VPCorrectReject": "VP Correct Reject",
    "system.cpu.valuePred.VPaccuracy": "VP Accuracy",
    "system.cpu.valuePred.VPcoverage": "VP Coverage",

    # DRAM / Bandwidth
    "system.mem_ctrl.dram.bytesRead::total": "DRAM Bytes Read",
    "system.mem_ctrl.dram.bytesWritten::total": "DRAM Bytes Written"
}

def parse_stats_file(stats_path: Path) -> dict:
    """Parses a gem5 stats.txt file and extracts keys matching TARGET_STATS."""
    extracted = {header: "" for header in TARGET_STATS.values()}

    if not stats_path.exists():
        return extracted

    with stats_path.open('r') as f:
        for line in f:
            line = line.strip()
            # Skip empty lines or header/footer lines
            if not line or line.startswith("---"):
                continue

            parts = line.split()
            if len(parts) >= 2:
                stat_name = parts[0]

                if stat_name in TARGET_STATS:
                    header = TARGET_STATS[stat_name]
                    value = parts[1]

                    # Basic float conversion to clean up formatting where possible
                    try:
                        if value != "nan":
                            # If it looks like an int, cast it to int, else float
                            value = int(value) if value.isdigit() else float(value)
                    except ValueError:
                        pass

                    extracted[header] = value

    return extracted

def select_sim_dir(value: Path | None) -> Path:
    """Selects the simulation directory, defaulting to the newest run with a manifest."""
    if value:
        candidate = Path(value)
        if not candidate.is_absolute():
            # Try as a subdirectory of SIM_ROOT, otherwise fall back to relative to CWD
            sim_root_candidate = SIM_ROOT / candidate
            if sim_root_candidate.is_dir():
                return sim_root_candidate.resolve()
        if not candidate.is_dir():
            print(f"Error: Simulation directory not found: {candidate}", file=sys.stderr)
            sys.exit(1)
        return candidate.resolve()

    if not SIM_ROOT.exists() or not SIM_ROOT.is_dir():
        print(f"Error: Simulation root directory does not exist: {SIM_ROOT}", file=sys.stderr)
        sys.exit(1)

    # Find the newest run directory containing run_manifest.json
    candidates = sorted(
        [d for d in SIM_ROOT.iterdir() if d.is_dir() and (d / "run_manifest.json").exists()]
    )
    if not candidates:
        print(f"Error: No simulation directories containing run_manifest.json found in {SIM_ROOT}", file=sys.stderr)
        sys.exit(1)
    
    selected = candidates[-1]
    print(f"Using newest simulation directory: {selected}")
    return selected.resolve()

def sanity_check_runs(sim_dir: Path, runs: list[dict]) -> None:
    """Sanity checks the directories and files on disk against the manifest runs."""
    expected_outdirs = {Path(run["outdir"]).resolve() for run in runs}
    
    actual_outdirs = set()
    if sim_dir.is_dir():
        for p_dir in sim_dir.iterdir():
            if p_dir.is_dir() and not p_dir.name.startswith('.'):
                for l_dir in p_dir.iterdir():
                    if l_dir.is_dir() and not l_dir.name.startswith('.'):
                        actual_outdirs.add(l_dir.resolve())

    missing_dirs = expected_outdirs - actual_outdirs
    unexpected_dirs = actual_outdirs - expected_outdirs

    if missing_dirs:
        print(f"Warning: {len(missing_dirs)} run directories expected by the manifest do not exist on disk.", file=sys.stderr)
        for d in sorted(list(missing_dirs))[:5]:
            print(f"  - Missing: {d.relative_to(sim_dir.parent)}", file=sys.stderr)
        if len(missing_dirs) > 5:
            print(f"  - ... and {len(missing_dirs) - 5} more.", file=sys.stderr)

    if unexpected_dirs:
        print(f"Warning: Found {len(unexpected_dirs)} directories on disk that are not listed in the manifest.", file=sys.stderr)
        for d in sorted(list(unexpected_dirs))[:5]:
            print(f"  - Unexpected: {d.relative_to(sim_dir.parent)}", file=sys.stderr)
        if len(unexpected_dirs) > 5:
            print(f"  - ... and {len(unexpected_dirs) - 5} more.", file=sys.stderr)

    # Check for finished runs missing stats.txt
    finished_but_no_stats = []
    for run in runs:
        if run.get("status") == "finished":
            outdir = Path(run["outdir"])
            stats_txt = outdir / "stats.txt"
            if not stats_txt.is_file():
                finished_but_no_stats.append(f"{run['profile']} / {run.get('config_label', 'unknown')}")

    if finished_but_no_stats:
        print(f"Warning: {len(finished_but_no_stats)} runs are marked 'finished' in manifest but missing 'stats.txt'.", file=sys.stderr)
        for item in finished_but_no_stats[:5]:
            print(f"  - Missing stats: {item}", file=sys.stderr)
        if len(finished_but_no_stats) > 5:
            print(f"  - ... and {len(finished_but_no_stats) - 5} more.", file=sys.stderr)

def main():
    parser = argparse.ArgumentParser(description="Extract gem5 stats to CSV.")
    parser.add_argument(
        "--sim-dir",
        type=Path,
        help="Path to the timestamped simulation directory (e.g., results/gem5_sim_runs/2026-07-10_14-00-00). Defaults to the newest run."
    )
    args = parser.parse_args()

    sim_dir = select_sim_dir(args.sim_dir)

    manifest_path = sim_dir / "run_manifest.json"
    if not manifest_path.exists():
        print(f"Error: Could not find {manifest_path}", file=sys.stderr)
        return 1

    with manifest_path.open('r') as f:
        manifest = json.load(f)

    runs = manifest.get("runs", [])
    if not runs:
        print("Error: No runs found in manifest.", file=sys.stderr)
        return 1

    # Sanity check the directories on disk against the manifest
    sanity_check_runs(sim_dir, runs)

    # Extract metadata headers dynamically from the first run in the manifest,
    # excluding execution/internal-specific details.
    excluded_fields = {"profile_path", "profile_status", "label", "returncode", "error", "outdir", "cwd", "command"}
    metadata_headers = [key for key in runs[0].keys() if key not in excluded_fields]
    print(f"Extracted metadata columns from manifest: {metadata_headers}")

    stat_headers = list(TARGET_STATS.values())
    all_headers = metadata_headers + stat_headers

    csv_out_path = sim_dir / "consolidated_results.csv"
    results = []

    with csv_out_path.open('w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=all_headers)
        writer.writeheader()

        valid_runs = 0
        for run in runs:
            row_data = {key: run.get(key, "") for key in metadata_headers}
            
            # Ensure essential fields for compute_summary exist
            row_data["profile"] = run.get("profile", "")
            row_data["config_label"] = run.get("config_label", "")
            row_data["status"] = run.get("status", "")

            stats_txt = Path(run["outdir"]) / "stats.txt"
            extracted_stats = parse_stats_file(stats_txt)
            row_data.update(extracted_stats)

            writer.writerow(row_data)
            results.append(row_data)
            valid_runs += 1

    print(f"Successfully extracted data for {valid_runs} runs.")
    print(f"Consolidated CSV saved to: {csv_out_path}")
    
    compute_summary(results, sim_dir)
    return 0

def compute_summary(results: list[dict], output_dir: Path) -> None:
    """Compute per-config aggregates and write summary CSV."""
    by_config: dict[str, list[dict]] = {}
    for r in results:
        by_config.setdefault(r["config_label"], []).append(r)

    stock_ipc: dict[str, float] = {}
    for r in results:
        if r["config_label"] == "stock":
            ipc = r.get("IPC")
            if ipc != "" and ipc is not None and float(ipc) > 0:
                stock_ipc[r["profile"]] = float(ipc)

    summary_rows = []
    for label, runs in by_config.items():
        ipcs = []
        accs = []
        coverages = []
        speedups = []
        
        for r in runs:
            ipc_val = r.get("IPC")
            if ipc_val != "" and ipc_val is not None:
                ipc_float = float(ipc_val)
                if ipc_float > 0:
                    ipcs.append(ipc_float)
                    sipc = stock_ipc.get(r["profile"])
                    if sipc:
                        speedups.append(ipc_float / sipc)
            
            acc_val = r.get("VP Accuracy")
            if acc_val != "" and acc_val is not None:
                acc_float = float(acc_val)
                if acc_float >= 0:
                    accs.append(acc_float)
                    
            cov_val = r.get("VP Coverage")
            if cov_val != "" and cov_val is not None:
                cov_float = float(cov_val)
                if cov_float >= 0:
                    coverages.append(cov_float)

        row = {
            "config_label": label,
            "n_workloads": len(ipcs),
            "mean_ipc": sum(ipcs) / len(ipcs) if ipcs else 0.0,
            "geomean_ipc": math.exp(sum(math.log(x) for x in ipcs) / len(ipcs)) if ipcs else 0.0,
            "mean_speedup": sum(speedups) / len(speedups) if speedups else 0.0,
            "geomean_speedup": math.exp(sum(math.log(x) for x in speedups) / len(speedups)) if speedups else 0.0,
            "mean_accuracy": sum(accs) / len(accs) if accs else 0.0,
            "mean_coverage": sum(coverages) / len(coverages) if coverages else 0.0,
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

if __name__ == "__main__":
    sys.exit(main())
