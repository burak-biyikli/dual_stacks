#!/usr/bin/env python3
"""
Extract key statistics from gem5 simulation runs using run_manifest.json.
Outputs a consolidated CSV suitable for Google Sheets analysis.
"""

import argparse
import csv
import json
import sys
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
    "system.cpu.valuePred.VPsupported": "VP Supported",
    "system.cpu.valuePred.VPpredicted": "VP Predicted",
    "system.cpu.valuePred.VPcorrect": "VP Correct",
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
                finished_but_no_stats.append(f"{run['profile']} / {run.get('label', 'unknown')}")

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

    with csv_out_path.open('w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=all_headers)
        writer.writeheader()

        valid_runs = 0
        for run in runs:
            row_data = {key: run.get(key, "") for key in metadata_headers}

            stats_txt = Path(run["outdir"]) / "stats.txt"
            extracted_stats = parse_stats_file(stats_txt)
            row_data.update(extracted_stats)

            writer.writerow(row_data)
            valid_runs += 1

    print(f"Successfully extracted data for {valid_runs} runs.")
    print(f"Consolidated CSV saved to: {csv_out_path}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
