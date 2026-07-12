#!/usr/bin/env python3
"""Run manifest-driven gem5 macrobenchmark sensitivity studies.

Use ``--dry-run`` first. It resolves profiles, workload commands, and all CPU
parameters without patching, building, or executing any external program.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shlex
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any
import itertools


SCRIPT_DIR = Path(__file__).resolve().parent
ROOT_DIR = SCRIPT_DIR.parent
EXT_DIR = ROOT_DIR / "ext"
GEM5_DIR = EXT_DIR / "gem5"
GEM5_BIN = GEM5_DIR / "build/X86/gem5.opt"
PROFILE_ROOT = ROOT_DIR / "results/dr_tool_runs"
SIM_ROOT = ROOT_DIR / "results/gem5_sim_runs"

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


def parse_csv(value: str) -> set[str]:
    return {item.strip() for item in value.split(",") if item.strip()}


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


def workload_command(profile: Profile, launcher: list[str], threads: int, outdir: Path) -> tuple[list[str], Path]:
    if profile.suite == "gapbs":
        binary = EXT_DIR / "gapbs" / profile.benchmark
        command = launcher + [str(binary), "-g", str(SIZE_MAP[profile.size]["gapbs"]), "-n", str(threads)]
        if profile.benchmark == "sssp":
            command.extend(("-d", "2"))
        return command, ROOT_DIR
    parsecmgmt = EXT_DIR / "parsec-benchmark/bin/parsecmgmt"
    launcher_text = shlex.join(launcher)
    command = [
        str(parsecmgmt), "-a", "run", "-p", profile.benchmark, "-c", "gcc",
        "-i", SIZE_MAP[profile.size]["parsec"], "-n", str(threads),
        "-d", str(outdir), "-s", launcher_text
    ]
    return command, EXT_DIR / "parsec-benchmark"


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
    
    # We dynamically extract all keys from the first run record, to act as CSV headers
    if manifest["runs"]:
        # Put status, returncode, error, profile, config_label, experiment first for readability
        first_cols = ["profile", "config_label", "experiment", "status", "returncode", "error"]
        other_cols = sorted(list(set(manifest["runs"][0].keys()) - set(first_cols) - {"profile_path", "profile_status", "outdir", "cwd", "command"}))
        fields = first_cols + other_cols + ["outdir", "command"]
    else:
        fields = []
        
    with (output_dir / "run_manifest.csv").open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(manifest["runs"])


def load_experiments(experiment_file: Path) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    with open(experiment_file) as f:
        data = json.load(f)
    
    globals_dict = data.get("globals", {})
    runs = []
    
    for exp in data.get("experiments", []):
        name = exp.get("name", "unnamed")
        matrix = exp.get("matrix", {})
        include = exp.get("include", [])
        
        keys = list(matrix.keys())
        value_lists = [matrix[k] for k in keys]
        
        combinations = []
        for prod in itertools.product(*value_lists):
            comb = dict(zip(keys, prod))
            combinations.append(comb)
            
        if not combinations:
            combinations = [{}]
            
        flat_runs = []
        if include:
            for comb in combinations:
                for inc in include:
                    merged = {**comb, **inc}
                    flat_runs.append(merged)
        else:
            flat_runs = combinations
            
        for run_params in flat_runs:
            full_params = {**globals_dict, **run_params}
            full_params["experiment"] = name
            
            if "lsu_ports" in full_params:
                lports, sports = map(int, str(full_params["lsu_ports"]).split(":"))
                full_params["load_ports"] = lports
                full_params["store_ports"] = sports
                del full_params["lsu_ports"]
                
            runs.append(full_params)
            
    return runs, globals_dict


def get_config_label(run_dict: dict[str, Any], global_defaults: dict[str, Any]) -> str:
    mr_mode = run_dict.get("mr_mode", "off")
    physreg = run_dict.get("physreg", 180)
    lq = run_dict.get("lq_entries", 72)
    sq = run_dict.get("sq_entries", 56)
    load_ports = run_dict.get("load_ports", 2)
    store_ports = run_dict.get("store_ports", 1)
    num_cores = run_dict.get("num_cores", 1)
    
    if mr_mode == "off":
        if physreg == 180 and lq == 72 and sq == 56 and load_ports == 2 and store_ports == 1:
            base = "stock"
        else:
            parts = []
            if physreg != 180:
                parts.append(f"prf_{physreg}")
            if load_ports != 2 or store_ports != 1:
                parts.append(f"bw_{load_ports}x{store_ports}")
            if lq != 72 or sq != 56:
                parts.append(f"lq{lq}_sq{sq}")
            base = "stock_" + "_".join(parts) if parts else "stock"
    else:
        log_max_conf = run_dict.get("mrp_log_max_confidence")
        pred_thresh = run_dict.get("mrp_prediction_threshold")
        alloc_conf = run_dict.get("mrp_allocation_confidence")
        demotion = run_dict.get("mrp_demotion_penalty")
        realloc = run_dict.get("mrp_realloc_penalty")
        reinit = run_dict.get("mrp_realloc_reinit")
        
        if (mr_mode == "full" and log_max_conf == 2 and pred_thresh == 3 and 
            alloc_conf == 1 and demotion == 1 and realloc == 1 and not reinit):
            base = "default_2bit"
        elif log_max_conf is None and pred_thresh is None and alloc_conf is None and demotion is None and realloc is None and reinit is None:
            if mr_mode == "static":
                if lq == 72 and sq == 56:
                    base = "mr_static"
                else:
                    base = f"mr_static_lq{lq}_sq{sq}"
            elif mr_mode == "full":
                if lq == 72 and sq == 56:
                    base = "mr_full"
                else:
                    base = f"mr_full_lq{lq}_sq{sq}"
            else:
                base = "mr_unknown"
        else:
            bits = f"{log_max_conf}bit" if log_max_conf is not None else "mr"
            parts = [bits]
            if pred_thresh is not None:
                parts.append(f"t{pred_thresh}")
            if alloc_conf is not None:
                parts.append(f"a{alloc_conf}")
            if demotion is not None:
                parts.append(f"dp{demotion}")
            if realloc is not None:
                parts.append(f"rp{realloc}")
            if reinit is not None:
                parts.append(f"ri{reinit}")
            
            if lq != 72 or sq != 56:
                parts.append(f"lq{lq}_sq{sq}")
            if physreg != 180:
                parts.append(f"prf{physreg}")
                
            base = "_".join(parts)
            
    if num_cores > 1:
        base += f"_core{num_cores}"
    return base


def build_gem5_args(run_dict: dict[str, Any]) -> list[str]:
    mapping = {
        "mr_mode": "--mr-mode",
        "physreg": "--physreg",
        "lq_entries": "--lq-entries",
        "sq_entries": "--sq-entries",
        "load_ports": "--load-ports",
        "store_ports": "--store-ports",
        "max_insts": "--max-insts",
        "num_cores": "--num-cores",
        "mrp_log_max_confidence": "--log-max-confidence",
        "mrp_prediction_threshold": "--prediction-threshold",
        "mrp_allocation_confidence": "--allocation-confidence",
        "mrp_realloc_penalty": "--realloc-penalty",
        "mrp_realloc_reinit": "--realloc-reinit",
        "mrp_demotion_penalty": "--demotion-penalty",
    }
    args = []
    for key, flag in mapping.items():
        if key in run_dict and run_dict[key] is not None:
            args.extend([flag, str(run_dict[key])])
    return args


def execute_one(record: dict[str, Any], timeout: int) -> tuple[str, int | None, str | None]:
    outdir = Path(record["outdir"])
    outdir.mkdir(parents=True, exist_ok=True)
    command = shlex.split(record["command"])
    cwd = Path(record["cwd"])
    
    threads = record.get("threads", 1)
    env = {**os.environ, "OMP_NUM_THREADS": str(threads)}
    
    with (outdir / "stdout.txt").open("w") as out, \
         (outdir / "stderr.txt").open("w") as err:
        try:
            proc = subprocess.run(command, cwd=cwd, stdout=out, stderr=err,
                                  timeout=timeout, check=False, env=env)
            status = "finished" if proc.returncode == 0 else "failed"
            returncode = proc.returncode
            error = f"process exited {returncode}" if returncode != 0 else None
        except subprocess.TimeoutExpired:
            status = "timed_out"
            returncode = None
            error = f"exceeded {timeout}s wall-clock timeout"
            
    return status, returncode, error


def get_default_jobs(mem_per_job_gb: float) -> int:
    try:
        pagesize = os.sysconf('SC_PAGE_SIZE')
        pages = os.sysconf('SC_PHYS_PAGES')
        total_mem_bytes = pagesize * pages
    except (AttributeError, ValueError):
        total_mem_bytes = 8 * 1024 * 1024 * 1024  # Default fallback 8GB
        
    total_mem_gb = total_mem_bytes / (1024 * 1024 * 1024)
    mem_jobs = int(total_mem_gb // mem_per_job_gb)
    cpu_jobs = os.cpu_count() or 1
    
    # Default to at least 1 job
    return max(1, min(cpu_jobs, mem_jobs))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment-file", default="baseline_sweeps.json", help="Path to the JSON experiment config file")
    parser.add_argument("--profile-dir", help="timestamp directory under results/dr_tool_runs (defaults to latest)")
    parser.add_argument("--sizes", default="all", help="comma-separated sizes, or all")
    parser.add_argument("--benchmarks", default="", help="comma-separated benchmark or profile-directory names")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--jobs", "-j", type=int, default=None, help="number of parallel simulation jobs to run (default: min(nproc, sys_mem/mem-per-job))")
    parser.add_argument("--mem-per-job", type=float, default=2.5, help="Estimated memory required per simulation job in GB (default: 3.0)")
    parser.add_argument("--skip-build", action="store_true", help="reuse an existing gem5 binary without applying patches or running SCons")
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
    
    if args.jobs is None:
        args.jobs = get_default_jobs(args.mem_per_job)
        
    for option in ("timeout", "jobs"):
        if getattr(args, option) <= 0:
            parser.error(f"--{option.replace('_', '-')} must be positive")
    return args


def main() -> int:
    args = parse_args()
    
    experiment_file = Path(args.experiment_file)
    if not experiment_file.is_absolute() and not experiment_file.is_file():
        # Fallback to the standard sweep_configs directory
        experiment_file = ROOT_DIR / "configs/sweep_configs" / args.experiment_file
    if not experiment_file.is_absolute():
        experiment_file = ROOT_DIR / experiment_file
    if not experiment_file.is_file():
        print(f"Experiment JSON file not found: {experiment_file}", file=sys.stderr)
        return 1
        
    experiment_configs, globals_dict = load_experiments(experiment_file)
    
    profile_dir = select_profile_dir(args.profile_dir)
    profiles, skipped = load_profiles(profile_dir, args.sizes, args.benchmarks)
    if not profiles:
        print("No eligible profiles selected.", file=sys.stderr)
        return 1
        
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    output_dir = Path(args.output_dir) if args.output_dir else SIM_ROOT / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)
    
    manifest: dict[str, Any] = {
        "created_at": timestamp,
        "dry_run": args.dry_run,
        "profile_dir": str(profile_dir),
        "config": "dynamic",
        "arguments": {key: value for key, value in vars(args).items() if key not in {"sizes", "benchmarks"}},
        "sizes": sorted(args.sizes),
        "benchmarks": sorted(args.benchmarks),
        "skipped_profiles": skipped,
        "runs": [],
    }
    
    for profile in profiles:
        for run_params in experiment_configs:
            params = dict(run_params)
            
            gem5_cfg_name = params.get("gem5_config", "single")
            num_cores = params.get("num_cores", 1)
            threads = params.get("threads", 1)
            
            if gem5_cfg_name not in CONFIGS:
                print(f"Error: Unknown gem5_config '{gem5_cfg_name}'", file=sys.stderr)
                sys.exit(1)
                
            if gem5_cfg_name in ("single", "mock") and num_cores != 1:
                print(f"Error: {gem5_cfg_name} config requires num_cores=1, got {num_cores}", file=sys.stderr)
                sys.exit(1)
                
            if threads != num_cores:
                print(f"Warning: OpenMP threads ({threads}) != num_cores ({num_cores}). This may cause artificial scheduling overhead.", file=sys.stderr)
                
            config_label = get_config_label(params, globals_dict)
            outdir = output_dir / profile.name / config_label
            
            gem5_args = build_gem5_args(params)
            gem5_config_path = CONFIGS[gem5_cfg_name]
            launcher = [str(GEM5_BIN), f"--outdir={outdir}", str(gem5_config_path)] + gem5_args
            
            command, cwd = workload_command(profile, launcher, threads, outdir)
            
            record = {
                "profile": profile.name,
                "profile_path": str(profile.path),
                "profile_status": profile.status,
                "config_label": config_label,
                "status": "planned",
                "returncode": None,
                "error": None,
                "outdir": str(outdir),
                "cwd": str(cwd),
                "command": shlex.join(command),
                **params
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
    completed_count = 0
    
    if args.jobs <= 1:
        for record in manifest["runs"]:
            completed_count += 1
            status, returncode, error = execute_one(record, args.timeout)
            record.update(status=status, returncode=returncode, error=error)
            status_icon = "✓" if status == "finished" else "✗"
            print(f"  [{completed_count}/{len(manifest['runs'])}] {status_icon} {record['profile']} / {record['config_label']}")
            if status != "finished":
                failures += 1
            write_manifests(output_dir, manifest)
    else:
        print(f"\nLaunching {len(manifest['runs'])} simulations across {args.jobs} workers...")
        with ProcessPoolExecutor(max_workers=args.jobs) as executor:
            future_to_record = {}
            for record in manifest["runs"]:
                future = executor.submit(execute_one, record, args.timeout)
                future_to_record[future] = record
                
            for future in as_completed(future_to_record):
                completed_count += 1
                record = future_to_record[future]
                status, returncode, error = future.result()
                record.update(status=status, returncode=returncode, error=error)
                status_icon = "✓" if status == "finished" else "✗"
                print(f"  [{completed_count}/{len(manifest['runs'])}] {status_icon} {record['profile']} / {record['config_label']}")
                if status != "finished":
                    failures += 1
                write_manifests(output_dir, manifest)
                
    print(f"Completed {len(manifest['runs'])} simulations; failures: {failures}.")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
