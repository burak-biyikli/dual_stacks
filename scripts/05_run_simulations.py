#!/usr/bin/env python3
"""Run manifest-driven gem5 macrobenchmark sensitivity studies.

Use ``--dry-run`` first. It resolves profiles, workload commands, and all CPU
parameters without patching, building, or executing any external program.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import shlex
import subprocess
import sys
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
EXT_DIR = ROOT_DIR / "ext"
GEM5_DIR = EXT_DIR / "gem5"
GEM5_BIN = GEM5_DIR / "build/X86/gem5.opt"
PROFILE_ROOT = ROOT_DIR / "results/dr_tool_runs"
SIM_ROOT = ROOT_DIR / "results/gem5_sim_runs"

BASE_PHYSREG = 180
BASE_LQ = 72
BASE_SQ = 56
DEFAULT_PORTS = (2, 1)
SIZE_MAP = {
    "test": {"parsec": "test", "gapbs": 10},
    "small": {"parsec": "simsmall", "gapbs": 15},
    "medium": {"parsec": "simmedium", "gapbs": 18},
    "large": {"parsec": "simlarge", "gapbs": 20},
    "native": {"parsec": "native", "gapbs": 22},
}
CONFIGS = {
    "mock": ROOT_DIR / "configs/run_o3_benchmark_mock.py",
    "single": ROOT_DIR / "configs/run_o3_benchmark_single.py",
    "multicore": ROOT_DIR / "configs/run_o3_benchmark_multicore.py",
}


@dataclass(frozen=True)
class Profile:
    name: str
    suite: str
    benchmark: str
    size: str
    path: Path
    status: str
    data: dict[str, Any]


@dataclass(frozen=True)
class RunSpec:
    label: str
    mr_mode: str
    physreg: int
    lq_entries: int
    sq_entries: int
    load_ports: int
    store_ports: int


def parse_csv(value: str) -> set[str]:
    return {item.strip() for item in value.split(",") if item.strip()}


def parse_ports(value: str) -> list[tuple[int, int]]:
    pairs = []
    for item in value.split(","):
        try:
            load, store = (int(part) for part in item.strip().split(":"))
        except ValueError as error:
            raise argparse.ArgumentTypeError("--lsu-ports entries must be LOAD:STORE") from error
        if load <= 0 or store <= 0:
            raise argparse.ArgumentTypeError("LSU ports must be positive")
        pairs.append((load, store))
    if not pairs:
        raise argparse.ArgumentTypeError("at least one LSU port pair is required")
    return pairs


def split_profile_name(name: str) -> tuple[str, str, str] | None:
    for suite in ("gapbs", "parsec"):
        prefix = f"{suite}_"
        if name.startswith(prefix):
            body = name[len(prefix):]
            benchmark, separator, size = body.rpartition("_")
            if separator and benchmark and size in SIZE_MAP:
                return suite, benchmark, size
    return None


def select_profile_dir(value: str | None) -> Path:
    if value:
        candidate = Path(value)
        if not candidate.is_absolute():
            candidate = PROFILE_ROOT / candidate
        if not candidate.is_dir():
            raise FileNotFoundError(f"profile directory not found: {candidate}")
        return candidate
    candidates = sorted(path for path in PROFILE_ROOT.iterdir() if path.is_dir())
    if not candidates:
        raise FileNotFoundError(f"no profile runs found in {PROFILE_ROOT}")
    return candidates[-1]


def load_profiles(profile_dir: Path, sizes: set[str], benchmarks: set[str]) -> tuple[list[Profile], list[dict[str, str]]]:
    profiles: list[Profile] = []
    skipped: list[dict[str, str]] = []
    for json_path in sorted(profile_dir.glob("*/parsed_data.json")):
        parsed = split_profile_name(json_path.parent.name)
        if not parsed:
            skipped.append({"profile": str(json_path.parent), "reason": "unrecognized profile directory name"})
            continue
        suite, benchmark, size = parsed
        full_name = json_path.parent.name
        if sizes and size not in sizes:
            continue
        aliases = {benchmark, full_name, f"{suite}_{benchmark}"}
        if benchmarks and not aliases.intersection(benchmarks):
            continue
        try:
            data = json.loads(json_path.read_text())
        except (OSError, json.JSONDecodeError) as error:
            skipped.append({"profile": str(json_path.parent), "reason": f"invalid JSON: {error}"})
            continue
        memory_ops = data.get("memory_ops", {})
        histogram = data.get("histogram", {})
        if memory_ops.get("total", 0) <= 0 or not histogram:
            skipped.append({"profile": str(json_path.parent), "reason": "missing memory counters or lifetime histogram"})
            continue
        profiles.append(Profile(full_name, suite, benchmark, size, json_path.parent, data.get("run_status", "unknown"), data))
    return profiles, skipped


def histogram_bins(profile: Profile) -> list[tuple[int, int]]:
    bins: list[tuple[int, int]] = []
    for key, count in profile.data.get("histogram", {}).items():
        try:
            index = int(key)
        except ValueError:
            index = 2048 if key.strip().startswith(">") else -1
        if index >= 0 and int(count) > 0:
            bins.append((index, int(count)))
    return sorted(bins)


def resolve_physregs(spec: str, profile: Profile, min_physreg: int | None) -> list[int]:
    fixed: list[int] = []
    percentages: list[float] = []
    for token in (part.strip() for part in spec.split(",")):
        if token.endswith("%"):
            try:
                percentage = float(token[:-1]) / 100
            except ValueError as error:
                raise ValueError(f"invalid PRF percentage: {token}") from error
            if not 0 < percentage <= 1:
                raise ValueError(f"PRF percentage must be in (0, 100]: {token}")
            percentages.append(percentage)
        else:
            try:
                value = int(token)
            except ValueError as error:
                raise ValueError(f"invalid PRF value: {token}") from error
            if value <= 0:
                raise ValueError("PRF values must be positive")
            fixed.append(value)
    if not fixed:
        raise ValueError("--physreg requires at least one explicit integer value")
    lower = min_physreg if min_physreg is not None else min(fixed)
    upper = max(fixed)
    values = set(fixed)
    values.add(BASE_PHYSREG)
    bins = histogram_bins(profile)
    total = sum(count for _, count in bins)
    for percentage in percentages:
        target = math.ceil(total * percentage)
        cumulative = 0
        cutoff = None
        for index, count in bins:
            cumulative += count
            if cumulative >= target:
                cutoff = index
                break
        if cutoff is not None:
            candidate = BASE_PHYSREG - cutoff
            if lower <= candidate <= upper:
                values.add(candidate)
    return sorted(values)


def trace_queue_sizes(profile: Profile, minimum: int) -> tuple[int, int]:
    memory = profile.data.get("memory_ops", {})
    stack = profile.data.get("stack_ops", {})
    stack_total = stack.get("total", 0)
    private_fraction = (stack.get("provably_private", 0) / stack_total) if stack_total else 0.0
    estimated_private_pops = memory.get("pops", 0) * private_fraction
    estimated_private_pushes = memory.get("pushes", 0) * private_fraction
    loads = memory.get("loads", 0)
    stores = memory.get("stores", 0)
    lq_fraction = max(0.0, 1.0 - estimated_private_pops / loads) if loads else 1.0
    sq_fraction = max(0.0, 1.0 - estimated_private_pushes / stores) if stores else 1.0
    return max(minimum, math.ceil(BASE_LQ * lq_fraction)), max(minimum, math.ceil(BASE_SQ * sq_fraction))


def build_run_specs(profile: Profile, args) -> list[RunSpec]:
    specs: list[RunSpec] = []
    for physreg in resolve_physregs(args.physreg, profile, args.min_physreg):
        specs.append(RunSpec(f"stock_prf_{physreg}", "off", physreg, BASE_LQ, BASE_SQ, *DEFAULT_PORTS))
    for load_ports, store_ports in args.lsu_ports:
        if (load_ports, store_ports) == DEFAULT_PORTS:
            continue
        specs.append(RunSpec(f"stock_bw_{load_ports}x{store_ports}", "off", BASE_PHYSREG, BASE_LQ, BASE_SQ, load_ports, store_ports))
    if not args.skip_mr_modes:
        specs.extend((
            RunSpec("mr_static", "static", BASE_PHYSREG, BASE_LQ, BASE_SQ, *DEFAULT_PORTS),
            RunSpec("mr_full", "full", BASE_PHYSREG, BASE_LQ, BASE_SQ, *DEFAULT_PORTS),
        ))
        lq, sq = trace_queue_sizes(profile, args.min_queue_entries)
        specs.append(RunSpec("mr_static_trace_lsu", "static", BASE_PHYSREG, lq, sq, *DEFAULT_PORTS))
    return specs


def config_arguments(spec: RunSpec, args) -> list[str]:
    values = [
        "--mr-mode", spec.mr_mode,
        "--physreg", str(spec.physreg),
        "--lq-entries", str(spec.lq_entries),
        "--sq-entries", str(spec.sq_entries),
        "--load-ports", str(spec.load_ports),
        "--store-ports", str(spec.store_ports),
        "--max-insts", str(args.max_insts),
    ]
    if args.config == "multicore":
        values.extend(("--num-cores", str(args.num_cores)))
    return values


def workload_command(profile: Profile, launcher: list[str], args) -> tuple[list[str], Path]:
    if profile.suite == "gapbs":
        binary = EXT_DIR / "gapbs" / profile.benchmark
        command = launcher + [str(binary), "-g", str(SIZE_MAP[profile.size]["gapbs"]), "-n", str(args.threads)]
        if profile.benchmark == "sssp":
            command.extend(("-d", "2"))
        return command, ROOT_DIR
    parsecmgmt = EXT_DIR / "parsec-benchmark/bin/parsecmgmt"
    launcher_text = shlex.join(launcher)
    command = [str(parsecmgmt), "-a", "run", "-p", profile.benchmark, "-c", "gcc", "-i", SIZE_MAP[profile.size]["parsec"], "-n", str(args.threads), "-s", launcher_text]
    return command, EXT_DIR / "parsec-benchmark"


def run_command(command: list[str], cwd: Path, stdout_path: Path, stderr_path: Path, timeout: int) -> tuple[str, int | None, str | None]:
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        try:
            completed = subprocess.run(command, cwd=cwd, stdout=stdout, stderr=stderr, timeout=timeout, check=False)
        except subprocess.TimeoutExpired:
            return "timed_out", None, f"exceeded {timeout}s wall-clock timeout"
    if completed.returncode:
        return "failed", completed.returncode, f"process exited {completed.returncode}"
    return "finished", 0, None


def apply_patches_and_build() -> None:
    commands = [
        ([str(SCRIPT_DIR / "00_patch_manager.sh"), "apply"], ROOT_DIR),
        (["scons", "build/X86/gem5.opt", f"-j{os.cpu_count() or 1}", "--linker=mold", "--ignore-style", "USE_CCACHE=1"], GEM5_DIR),
    ]
    for command, cwd in commands:
        print("[Running]", shlex.join(command))
        subprocess.run(command, cwd=cwd, check=True)


def write_manifests(output_dir: Path, manifest: dict[str, Any]) -> None:
    (output_dir / "run_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    fields = ["profile", "profile_status", "label", "mr_mode", "physreg", "lq_entries", "sq_entries", "load_ports", "store_ports", "status", "returncode", "error", "outdir", "command"]
    with (output_dir / "run_manifest.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(manifest["runs"])


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile-dir", help="timestamp directory under results/dr_tool_runs (defaults to latest)")
    parser.add_argument("--sizes", default="all", help="comma-separated sizes, or all")
    parser.add_argument("--benchmarks", default="", help="comma-separated benchmark or profile-directory names")
    parser.add_argument("--physreg", default="100,150,200,10%,15%")
    parser.add_argument("--min-physreg", type=int, help="minimum accepted histogram-derived integer PRF")
    parser.add_argument("--lsu-ports", type=parse_ports, default=parse_ports("1:1,2:1,3:2"))
    parser.add_argument("--min-queue-entries", type=int, default=4)
    parser.add_argument("--max-insts", type=int, default=1_000_000)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--config", choices=tuple(CONFIGS), default="single")
    parser.add_argument("--num-cores", type=int, default=4)
    parser.add_argument("--skip-build", action="store_true", help="reuse an existing gem5 binary without applying patches or running SCons")
    parser.add_argument("--skip-mr-modes", action="store_true", help="skip Memory Renaming (MR) configurations; run stock PRF and LSU bandwidth sweeps only")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--output-dir", help="output directory (defaults to timestamped results/gem5_sim_runs directory)")
    args = parser.parse_args()
    if args.sizes == "all":
        args.sizes = set(SIZE_MAP)
    else:
        args.sizes = parse_csv(args.sizes)
        unknown = args.sizes - set(SIZE_MAP)
        if unknown:
            parser.error(f"unknown sizes: {', '.join(sorted(unknown))}")
    args.benchmarks = parse_csv(args.benchmarks)
    for option in ("min_queue_entries", "max_insts", "timeout", "threads", "num_cores"):
        if getattr(args, option) <= 0:
            parser.error(f"--{option.replace('_', '-')} must be positive")
    if args.config == "multicore" and args.num_cores < 2:
        parser.error("--config multicore requires --num-cores >= 2")
    return args


def main() -> int:
    args = parse_args()
    profile_dir = select_profile_dir(args.profile_dir)
    profiles, skipped = load_profiles(profile_dir, args.sizes, args.benchmarks)
    if not profiles:
        print("No eligible profiles selected.", file=sys.stderr)
        return 1
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    output_dir = Path(args.output_dir) if args.output_dir else SIM_ROOT / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)
    config = CONFIGS[args.config]
    manifest: dict[str, Any] = {
        "created_at": timestamp,
        "dry_run": args.dry_run,
        "profile_dir": str(profile_dir),
        "config": str(config),
        "arguments": {key: value for key, value in vars(args).items() if key not in {"sizes", "benchmarks", "lsu_ports"}},
        "sizes": sorted(args.sizes),
        "benchmarks": sorted(args.benchmarks),
        "lsu_ports": [f"{load}:{store}" for load, store in args.lsu_ports],
        "skipped_profiles": skipped,
        "runs": [],
    }
    for profile in profiles:
        for spec in build_run_specs(profile, args):
            outdir = output_dir / profile.name / spec.label
            launcher = [str(GEM5_BIN), f"--outdir={outdir}", str(config)] + config_arguments(spec, args)
            command, cwd = workload_command(profile, launcher, args)
            record = {
                "profile": profile.name,
                "profile_path": str(profile.path),
                "profile_status": profile.status,
                **asdict(spec),
                "status": "planned",
                "returncode": None,
                "error": None,
                "outdir": str(outdir),
                "cwd": str(cwd),
                "command": shlex.join(command),
            }
            manifest["runs"].append(record)
    write_manifests(output_dir, manifest)
    print(f"Resolved {len(manifest['runs'])} simulations from {len(profiles)} profiles.")
    print(f"Manifest: {output_dir / 'run_manifest.json'}")
    if args.dry_run:
        return 0
    if args.skip_build:
        if not GEM5_BIN.is_file():
            print(f"gem5 binary not found: {GEM5_BIN}", file=sys.stderr)
            return 1
        print(f"Reusing existing gem5 binary: {GEM5_BIN}")
    else:
        try:
            apply_patches_and_build()
        except subprocess.CalledProcessError as error:
            print(f"Build failed: {error}", file=sys.stderr)
            for record in manifest["runs"]:
                record["status"] = "not_run"
                record["error"] = "gem5 build failed"
            write_manifests(output_dir, manifest)
            return error.returncode or 1
    failures = 0
    for record in manifest["runs"]:
        outdir = Path(record["outdir"])
        outdir.mkdir(parents=True, exist_ok=True)
        command = shlex.split(record["command"])
        status, returncode, error = run_command(command, Path(record["cwd"]), outdir / "stdout.txt", outdir / "stderr.txt", args.timeout)
        record.update(status=status, returncode=returncode, error=error)
        print(f"[{status}] {record['profile']} / {record['label']}")
        if status != "finished":
            failures += 1
        write_manifests(output_dir, manifest)
    print(f"Completed {len(manifest['runs'])} simulations; failures: {failures}.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
