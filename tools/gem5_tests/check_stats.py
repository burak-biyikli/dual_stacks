#!/usr/bin/env python3
import sys
import re
import os

if len(sys.argv) < 4 or len(sys.argv) > 5:
    print("Usage: check_stats.py <mode> <test_name> <stats_file> [stock_stats_file]")
    sys.exit(1)

mode = sys.argv[1]
test_name = sys.argv[2]
stats_file = sys.argv[3]
stock_file = sys.argv[4] if len(sys.argv) == 5 else None

if not os.path.exists(stats_file):
    print(f"Error: {stats_file} does not exist.")
    sys.exit(1)

# Regex to capture stat values
stat_pattern = re.compile(r'^\s*[\w\.]+\.VP(RenameSupported|RenamePredicted|Correct|Mispredict|MissedOpportunity|CorrectReject)\s+(\d+)')
ipc_pattern = re.compile(r'^\s*system\.cpu\.ipc\s+([\d\.]+)')

def parse_file(filepath):
    stats = {
        "VPRenameSupported": 0,
        "VPRenamePredicted": 0,
        "VPCorrect": 0,
        "VPMispredict": 0,
        "VPMissedOpportunity": 0,
        "VPCorrectReject": 0,
        "ipc": 0.0
    }
    if not filepath or not os.path.exists(filepath):
        return stats
    
    with open(filepath, 'r') as f:
        for line in f:
            m = stat_pattern.search(line)
            if m:
                stat_name = "VP" + m.group(1)
                stats[stat_name] += int(m.group(2))
            else:
                m_ipc = ipc_pattern.search(line)
                if m_ipc:
                    stats["ipc"] = float(m_ipc.group(1))
    return stats

mr_stats = parse_file(stats_file)
stock_stats = parse_file(stock_file)

if mode == "stock":
    for k in ["VPRenameSupported", "VPRenamePredicted", "VPCorrect", "VPMispredict", "VPMissedOpportunity", "VPCorrectReject"]:
        if mr_stats[k] != 0:
            print(f"    [FAIL] Stock run should have 0 {k}, got {mr_stats[k]}")
            sys.exit(1)
    print("    [PASS] Stock stats are clean (all 0 or missing).")

elif mode.startswith("mr"):
    label = "MR"
    if mode == "mr_penalty":
        label = "MR (penalty)"
    elif mode == "mr_reinit":
        label = "MR (Reinit)"

    supported = mr_stats["VPRenameSupported"]
    predicted = mr_stats["VPRenamePredicted"]
    correct = mr_stats["VPCorrect"]
    mispredict = mr_stats["VPMispredict"]
    missed = mr_stats["VPMissedOpportunity"]
    reject = mr_stats["VPCorrectReject"]

    # Verify invariant: Rename-predicted is an upper bound on committed predictions (accounting for squashes)
    assert predicted >= correct + mispredict, \
        f"VPRenamePredicted ({predicted}) < VPCorrect ({correct}) + VPMispredict ({mispredict})"

    accuracy_str = "0.00% Accuracy"
    total_committed_predictions = correct + mispredict
    if total_committed_predictions > 0:
        acc = (correct / total_committed_predictions) * 100
        accuracy_str = f"{acc:.2f}% Accuracy"

    coverage_str = "0.00% Coverage"
    total_opportunities = correct + missed
    if total_opportunities > 0:
        cov = (correct / total_opportunities) * 100
        coverage_str = f"{cov:.2f}% Coverage"

    speedup_str = "N/A IPC Change"
    if stock_file and stock_stats["ipc"] > 0:
        speedup = mr_stats["ipc"] / stock_stats["ipc"]
        speedup_str = f"{speedup:.5f}X IPC vs stock"

    if test_name in ["no_forwarding_dependency", "no_forwarding_size", "no_forwarding_demotion"]:
        if predicted != 0:
            print(f"    [FAIL] {label} on {test_name} expected 0 VPRenamePredicted, got {predicted}")
            sys.exit(1)
        print(f"    [PASS] {label} on {test_name} correctly avoided prediction.")
    else:
        if supported == 0 or predicted == 0:
            print(f"    [FAIL] {label} on {test_name} expected VPRenameSupported > 0 and VPRenamePredicted > 0, got {supported} and {predicted}")
            sys.exit(1)
        print(f"    [PASS] {label} on {test_name} showed active prediction (Supported={supported}, Predicted={predicted}, {accuracy_str}, {coverage_str}, {speedup_str}).")

sys.exit(0)