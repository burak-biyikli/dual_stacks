#!/usr/bin/env python3
"""Experimental multicore SE benchmark configuration.

This is deliberately best-effort only. It shares the single-core argument
contract, but workload placement and inter-thread sharing have not yet been
validated for publication-quality results.
"""

from benchmark_system import build_system, install_workload, parse_benchmark_arguments, run


args = parse_benchmark_arguments("EXPERIMENTAL multicore SE benchmark configuration")
if args.num_cores < 2:
    raise SystemExit("multicore configuration requires --num-cores >= 2")
print("WARNING: multicore SE configuration is unvalidated; use for construction tests only.")
system, cpus = build_system(args, cores=args.num_cores)
install_workload(system, cpus, args)
run(system)
