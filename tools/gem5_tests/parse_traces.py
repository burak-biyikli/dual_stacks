#!/usr/bin/env python3
import sys
import re
import os

if len(sys.argv) != 3:
    print("Usage: parse_traces.py <input_trace> <output_clean_trace>")
    sys.exit(1)

in_path = sys.argv[1]
out_path = sys.argv[2]

if not os.path.exists(in_path):
    print(f"Error: {in_path} does not exist.")
    sys.exit(1)

# Matches leading digits followed by a colon and space
tick_pattern = re.compile(r'^\s*\d+\s*:\s*(.*)')

with open(in_path, 'r') as fin, open(out_path, 'w') as fout:
    for line in fin:
        match = tick_pattern.match(line)
        if match:
            fout.write(match.group(1) + '\n')
        else:
            fout.write(line)
