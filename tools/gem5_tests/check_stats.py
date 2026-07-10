#!/usr/bin/env python3
import sys
import re
import os

if len(sys.argv) != 4:
    print("Usage: check_stats.py <mode> <test_name> <stats_file>")
    sys.exit(1)

mode = sys.argv[1]
test_name = sys.argv[2]
stats_file = sys.argv[3]

if not os.path.exists(stats_file):
    print(f"Error: {stats_file} does not exist.")
    sys.exit(1)

# Stats we care about
stats = {
    "VPsupported": 0,
    "VPpredicted": 0,
    "VPcorrect": 0,
}

# Regex to capture stat values. E.g. system.cpu.valuePred.stats.VPpredicted      10
stat_pattern = re.compile(r'^\s*[\w\.]+\.VP(supported|predicted|correct)\s+(\d+)')

with open(stats_file, 'r') as f:
    for line in f:
        m = stat_pattern.search(line)
        if m:
            stat_name = "VP" + m.group(1)
            stats[stat_name] += int(m.group(2))

if mode == "stock":
    for k, v in stats.items():
        if v != 0:
            print(f"    [FAIL] Stock run should have 0 {k}, got {v}")
            sys.exit(1)
    print("    [PASS] Stock stats are clean (all 0 or missing).")

elif mode == "mr":
    if test_name in ["no_forwarding_dependency", "no_forwarding_size", "no_forwarding_demotion" ]:
        if stats["VPpredicted"] != 0:
            print(f"    [FAIL] {test_name} expected 0 VPpredicted, got {stats['VPpredicted']}")
            sys.exit(1)
        print(f"    [PASS] MR mode on no_forwarding correctly avoided prediction.")
    else:
        if stats["VPsupported"] == 0 or stats["VPpredicted"] == 0:
            print(f"    [FAIL] MR mode on {test_name} expected VPsupported > 0 and VPpredicted > 0, got {stats['VPsupported']} and {stats['VPpredicted']}")
            sys.exit(1)
        print(f"    [PASS] MR mode on {test_name} showed active prediction (Supported={stats['VPsupported']}, Predicted={stats['VPpredicted']}, Correct={stats['VPcorrect']}).")

sys.exit(0)
