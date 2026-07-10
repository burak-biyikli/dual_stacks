#!/usr/bin/env python3
"""Supported single-core Skylake-like SE benchmark configuration."""

from benchmark_system import build_system, install_workload, parse_benchmark_arguments, run


args = parse_benchmark_arguments("Run one SE workload on the benchmark O3 core")
system, cpus = build_system(args, cores=1)
install_workload(system, cpus, args)
run(system)
