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
import signal
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

# ─── Parameter registry ────────────────────────────────────────────────
# To add a new sweep parameter, add an entry here.  build_gem5_args()
# and load_experiments() both derive their mappings from this table.
#
#   json_key:   key used in experiment JSON and run dicts
#   cli_flag:   gem5 config script command-line flag
#   required:   must be present in the experiment JSON "globals" section
#               (note: lsu_ports is split into load_ports/store_ports
#               before validation, so load_ports/store_ports are required
#               here while lsu_ports is validated separately)
PARAM_REGISTRY: list[tuple[str, str, bool]] = [
    # (json_key,                      cli_flag,                   required)
    ("mr_mode",                       "--mr-mode",                False),
    ("physreg",                       "--physreg",                True),
    ("lq_entries",                    "--lq-entries",             True),
    ("sq_entries",                    "--sq-entries",             True),
    ("load_ports",                    "--load-ports",             False),
    ("store_ports",                   "--store-ports",            False),
    ("max_insts",                     "--max-insts",              True),
    ("num_cores",                     "--num-cores",              True),
    ("mrp_log_max_confidence",        "--log-max-confidence",     False),
    ("mrp_prediction_threshold",      "--prediction-threshold",   False),
    ("mrp_allocation_confidence",     "--allocation-confidence",  False),
    ("mrp_realloc_penalty",           "--realloc-penalty",        False),
    ("mrp_realloc_reinit",            "--realloc-reinit",         False),
    ("mrp_demotion_penalty",          "--demotion-penalty",       False),
]


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
    
    # Validate that all required global baseline parameters are specified.
    # gem5_config, threads, and lsu_ports are always required but are not in
    # PARAM_REGISTRY (lsu_ports is split into load_ports/store_ports later).
    required_keys = {"gem5_config", "threads", "lsu_ports"} | {
        key for key, _, req in PARAM_REGISTRY if req
    }
    missing_keys = required_keys - set(globals_dict.keys())
    if missing_keys:
        raise ValueError(
            f"Invalid experiment configuration file '{experiment_file}': "
            f"The 'globals' section is missing required baseline parameters: {', '.join(sorted(missing_keys))}"
        )
        
    # Split baseline lsu_ports into load_ports and store_ports inside globals_dict
    lports, sports = map(int, str(globals_dict["lsu_ports"]).split(":"))
    globals_dict["load_ports"] = lports
    globals_dict["store_ports"] = sports
    del globals_dict["lsu_ports"]
    
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
    
    b_physreg = global_defaults["physreg"]
    b_lq = global_defaults["lq_entries"]
    b_sq = global_defaults["sq_entries"]
    b_load_ports = global_defaults["load_ports"]
    b_store_ports = global_defaults["store_ports"]
    
    physreg = run_dict.get("physreg", b_physreg)
    lq = run_dict.get("lq_entries", b_lq)
    sq = run_dict.get("sq_entries", b_sq)
    load_ports = run_dict.get("load_ports", b_load_ports)
    store_ports = run_dict.get("store_ports", b_store_ports)
    num_cores = run_dict.get("num_cores", 1)
    
    lq_str = "ts" if isinstance(lq, str) and lq.startswith("trace-scaled") else str(lq)
    sq_str = "ts" if isinstance(sq, str) and sq.startswith("trace-scaled") else str(sq)
    
    if mr_mode == "off":
        if physreg == b_physreg and lq == b_lq and sq == b_sq and load_ports == b_load_ports and store_ports == b_store_ports:
            base = "stock"
        else:
            parts = []
            if physreg != b_physreg:
                parts.append(f"prf_{physreg}")
            if load_ports != b_load_ports or store_ports != b_store_ports:
                parts.append(f"bw_{load_ports}x{store_ports}")
            if lq != b_lq or sq != b_sq:
                parts.append(f"lq{lq_str}_sq{sq_str}")
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
                if lq == b_lq and sq == b_sq:
                    base = "mr_static"
                else:
                    base = f"mr_static_lq{lq_str}_sq{sq_str}"
            elif mr_mode == "full":
                if lq == b_lq and sq == b_sq:
                    base = "mr_full"
                else:
                    base = f"mr_full_lq{lq_str}_sq{sq_str}"
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
            
            if lq != b_lq or sq != b_sq:
                parts.append(f"lq{lq_str}_sq{sq_str}")
            if physreg != b_physreg:
                parts.append(f"prf{physreg}")
                
            base = "_".join(parts)
            
    if num_cores > 1:
        base += f"_core{num_cores}"
    return base


def build_gem5_args(run_dict: dict[str, Any]) -> list[str]:
    """Build gem5 CLI arguments from a run dict using PARAM_REGISTRY."""
    args = []
    for key, flag, _ in PARAM_REGISTRY:
        if key in run_dict and run_dict[key] is not None:
            args.extend([flag, str(run_dict[key])])
    return args


def execute_one(record: dict[str, Any], timeout: int, grace_period: int = 30, output_dir: Path | None = None) -> tuple[str, int | None, str | None]:
    outdir = Path(record["outdir"])
    if not outdir.is_absolute() and output_dir is not None:
        outdir = output_dir / outdir
    outdir.mkdir(parents=True, exist_ok=True)
    command = shlex.split(record["command"])
    cwd = Path(record["cwd"])
    
    threads = record.get("threads", 1)
    env = {**os.environ, "OMP_NUM_THREADS": str(threads)}
    
    with (outdir / "stdout.txt").open("w") as out, \
         (outdir / "stderr.txt").open("w") as err:
        try:
            proc = subprocess.Popen(command, cwd=cwd, stdout=out, stderr=err, env=env,
                                    start_new_session=True)
            try:
                proc.wait(timeout=timeout)
                status = "finished" if proc.returncode == 0 else "failed"
                returncode = proc.returncode
                error = f"process exited {returncode}" if returncode != 0 else None
            except subprocess.TimeoutExpired:
                status = "timed_out"
                returncode = None
                error = f"exceeded {timeout}s wall-clock timeout"
                
                # Gracefully terminate via SIGINT to the process group (covers gem5 spawned by parsecmgmt)
                try:
                    os.killpg(proc.pid, signal.SIGINT)
                    proc.wait(timeout=max(1, grace_period // 2))
                except subprocess.TimeoutExpired:
                    # Send second SIGINT if it hasn't terminated yet
                    try:
                        os.killpg(proc.pid, signal.SIGINT)
                        proc.wait(timeout=max(1, grace_period - (grace_period // 2)))
                    except subprocess.TimeoutExpired:
                        # Force kill if still running
                        try:
                            os.killpg(proc.pid, signal.SIGKILL)
                            proc.wait(timeout=5)
                        except Exception:
                            pass
                    except Exception:
                        pass
                except (ProcessLookupError, PermissionError):
                    pass
        except Exception as e:
            status = "failed"
            returncode = None
            error = f"failed to execute or wait for process: {e}"
            
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
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Resume Behavior:
  Use --resume to resume a previous run. If --output-dir is not specified, the script
  will automatically find and resume the latest run directory under results/gem5_sim_runs/.
  A previous run is cached (skipped) if it has a non-empty stats.txt file, regardless
  of whether its status was "finished" or "timed_out". Failed runs are always re-run.
  Timed-out runs without a non-empty stats.txt are also re-run.
"""
    )
    parser.add_argument("--experiment-file", default="baseline_sweeps.json", help="Path to the JSON experiment config file")
    parser.add_argument("--profile-dir", help="timestamp directory under results/dr_tool_runs (defaults to latest)")
    parser.add_argument("--sizes", default="all", help="comma-separated sizes, or all")
    parser.add_argument("--benchmarks", default="", help="comma-separated benchmark or profile-directory names")
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--grace-period", type=int, default=30, help="Grace period in seconds for gem5 to clean up and dump stats after timeout (default: 30)")
    parser.add_argument("--jobs", "-j", type=int, default=None, help="number of parallel simulation jobs to run (default: min(nproc, sys_mem/mem-per-job))")
    parser.add_argument("--mem-per-job", type=float, default=2.5, help="Estimated memory required per simulation job in GB (default: 2.5)")
    parser.add_argument("--skip-build", action="store_true", help="reuse an existing gem5 binary without applying patches or running SCons")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--output-dir", help="output directory (defaults to timestamped results/gem5_sim_runs directory)")
    parser.add_argument("--resume", action="store_true",
                        help="Resume a previous run.  Defaults --output-dir to the latest "
                             "run directory.  Skips any run whose stats.txt already exists; "
                             "always re-runs failed runs, and re-runs timed-out runs only "
                             "when their stats.txt is missing.")
    
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
        
    for option in ("timeout", "jobs", "grace_period"):
        if getattr(args, option) <= 0:
            parser.error(f"--{option.replace('_', '-')} must be positive")
    return args


def resolve_trace_scaled_entries(params: dict[str, Any], profile: Profile, globals_dict: dict[str, Any]) -> None:
    """Resolve 'trace-scaled*' LQ/SQ entries to integer values using profile data.

    Mutates *params* in-place, replacing any ``trace-scaled*`` string value
    for ``lq_entries`` or ``sq_entries`` with the computed integer size.
    """
    base_lq = globals_dict["lq_entries"]
    base_sq = globals_dict["sq_entries"]

    lq_val = params.get("lq_entries")
    sq_val = params.get("sq_entries")

    if not ((isinstance(lq_val, str) and lq_val.startswith("trace-scaled")) or
            (isinstance(sq_val, str) and sq_val.startswith("trace-scaled"))):
        return

    memory_ops = profile.data.get("memory_ops", {})
    loads = memory_ops.get("loads", 1)
    stores = memory_ops.get("stores", 1)
    pushes = memory_ops.get("pushes", 0)
    pops = memory_ops.get("pops", 0)

    load_pop_ratio = pops / loads if loads > 0 else 0.0
    store_push_ratio = pushes / stores if stores > 0 else 0.0

    stack_ops = profile.data.get("stack_ops", {})
    stack_total = stack_ops.get("total", 1)

    def _private_percentage(spec: str) -> float:
        if spec == "trace-scaled-strict":
            selected_ipc = stack_ops.get("strictly_ipc", 0)
        elif spec == "trace-scaled-history":
            selected_ipc = stack_ops.get("possibly_ipc_hist", 0)
        elif spec == "trace-scaled-non-history":
            selected_ipc = stack_ops.get("possibly_ipc_non_hist", 0)
        else:  # default "trace-scaled"
            selected_ipc = stack_total - stack_ops.get("provably_private", 0)

        priv_pct = (stack_total - selected_ipc) / stack_total if stack_total > 0 else 0.0
        return min(max(0.0, priv_pct), 0.95)

    if isinstance(lq_val, str) and lq_val.startswith("trace-scaled"):
        lq_priv_pct = _private_percentage(lq_val)
        lq_factor = min(load_pop_ratio * lq_priv_pct, 0.95)
        params["lq_entries"] = int(base_lq / (1.0 - lq_factor))

    if isinstance(sq_val, str) and sq_val.startswith("trace-scaled"):
        sq_priv_pct = _private_percentage(sq_val)
        sq_factor = min(store_push_ratio * sq_priv_pct, 0.95)
        params["sq_entries"] = int(base_sq / (1.0 - sq_factor))


def report_completion(
    record: dict[str, Any],
    index: int,
    total: int,
    n_finished: int,
    n_failed: int,
    n_timed_out: int,
) -> tuple[int, int, int]:
    """Print a status line for a completed run and update counters."""
    status = record["status"]
    if status == "finished":
        status_icon = "✓"
        n_finished += 1
    elif status == "timed_out":
        status_icon = "~"
        n_timed_out += 1
    else:
        status_icon = "✗"
        n_failed += 1
    print(f"  [{index}/{total}] {status_icon} {record['profile']} / {record['config_label']}")
    return n_finished, n_failed, n_timed_out


def resolve_runs(
    profiles: list[Profile],
    experiment_configs: list[dict[str, Any]],
    globals_dict: dict[str, Any],
    output_dir: Path,
    profile_dir: Path,
    args: argparse.Namespace,
    timestamp: str,
    skipped: list[dict[str, str]],
) -> dict[str, Any]:
    manifest_path = output_dir / "run_manifest.json"
    if not args.resume and args.output_dir and manifest_path.is_file():
        raise ValueError(
            f"Output directory {output_dir} already exists and contains a run manifest. "
            f"To resume this run, use the --resume flag. Otherwise, specify a different --output-dir."
        )

    previous_runs = {}
    if args.resume and manifest_path.is_file():
        try:
            prev_manifest = json.loads(manifest_path.read_text())
            for r in prev_manifest.get("runs", []):
                key = (r["profile"], r.get("config_label", r.get("label", "")))
                previous_runs[key] = r
        except Exception:
            pass

    manifest: dict[str, Any] = {
        "created_at": timestamp,
        "dry_run": args.dry_run,
        "profile_dir": str(profile_dir.relative_to(ROOT_DIR)) if profile_dir.is_relative_to(ROOT_DIR) else str(profile_dir),
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
                raise ValueError(f"Unknown gem5_config '{gem5_cfg_name}'")
                
            if gem5_cfg_name in ("single", "mock") and num_cores != 1:
                raise ValueError(f"{gem5_cfg_name} config requires num_cores=1, got {num_cores}")
                
            if threads != num_cores:
                print(f"Warning: OpenMP threads ({threads}) != num_cores ({num_cores}). This may cause artificial scheduling overhead.", file=sys.stderr)
                
            config_label = get_config_label(params, globals_dict)
            resolve_trace_scaled_entries(params, profile, globals_dict)
            
            outdir = output_dir / profile.name / config_label
            
            gem5_args = build_gem5_args(params)
            gem5_config_path = CONFIGS[gem5_cfg_name]
            launcher = [str(GEM5_BIN), f"--outdir={outdir}", str(gem5_config_path)] + gem5_args
            
            command, cwd = workload_command(profile, launcher, threads, outdir)
            
            record = {
                "profile": profile.name,
                "profile_path": str(profile.path.relative_to(ROOT_DIR)) if profile.path.is_relative_to(ROOT_DIR) else str(profile.path),
                "profile_status": profile.status,
                "config_label": config_label,
                "status": "planned",
                "returncode": None,
                "error": None,
                "outdir": str(outdir.relative_to(output_dir)),
                "cwd": str(cwd),
                "command": shlex.join(command),
                **params
            }
            # Check if this run was already completed or has usable data
            prev_record = previous_runs.get((profile.name, config_label))
            if prev_record:
                prev_status = prev_record.get("status", "")
                stats_file = outdir / "stats.txt"
                has_stats = stats_file.is_file() and stats_file.stat().st_size > 0
                if prev_status == "failed":
                    # Always re-run failed runs
                    pass
                elif has_stats:
                    # Cache any run (finished or timed_out) that has usable data
                    record["status"] = prev_status
                    record["returncode"] = prev_record.get("returncode")
                    record["error"] = prev_record.get("error")
            manifest["runs"].append(record)
            
    # Print resume summary if there are cached runs
    n_cached = sum(1 for r in manifest["runs"] if r["status"] in ("finished", "timed_out"))
    n_planned = sum(1 for r in manifest["runs"] if r["status"] == "planned")
    if n_cached > 0:
        n_cached_finished = sum(1 for r in manifest["runs"] if r["status"] == "finished")
        n_cached_timedout = sum(1 for r in manifest["runs"] if r["status"] == "timed_out")
        parts = [f"{n_cached_finished} finished (cached)"]
        if n_cached_timedout:
            parts.append(f"{n_cached_timedout} timed_out (cached)")
        parts.append(f"{n_planned} planned")
        print(f"Resuming: {', '.join(parts)}.")
    
    write_manifests(output_dir, manifest)
    return manifest


def execute_runs(
    manifest: dict[str, Any],
    output_dir: Path,
    args: argparse.Namespace,
) -> tuple[int, int, int]:
    n_finished = 0
    n_failed = 0
    n_timed_out = 0
    completed_count = 0
    
    if args.jobs <= 1:
        for record in manifest["runs"]:
            completed_count += 1
            if record["status"] in ("finished", "timed_out"):
                if record["status"] == "finished":
                    n_finished += 1
                    status_icon = "✓"
                else:
                    n_timed_out += 1
                    status_icon = "~"
                print(f"  [{completed_count}/{len(manifest['runs'])}] {status_icon} {record['profile']} / {record['config_label']} (cached)")
                continue
            status, returncode, error = execute_one(record, args.timeout, args.grace_period, output_dir)
            record.update(status=status, returncode=returncode, error=error)
            n_finished, n_failed, n_timed_out = report_completion(
                record, completed_count, len(manifest["runs"]),
                n_finished, n_failed, n_timed_out)
            write_manifests(output_dir, manifest)
    else:
        print(f"\nLaunching {len(manifest['runs'])} simulations across {args.jobs} workers...")
        with ProcessPoolExecutor(max_workers=args.jobs) as executor:
            future_to_record = {}
            for record in manifest["runs"]:
                if record["status"] in ("finished", "timed_out"):
                    completed_count += 1
                    if record["status"] == "finished":
                        n_finished += 1
                        status_icon = "✓"
                    else:
                        n_timed_out += 1
                        status_icon = "~"
                    print(f"  [{completed_count}/{len(manifest['runs'])}] {status_icon} {record['profile']} / {record['config_label']} (cached)")
                    continue
                future = executor.submit(execute_one, record, args.timeout, args.grace_period, output_dir)
                future_to_record[future] = record
                
            if future_to_record:
                for future in as_completed(future_to_record):
                    completed_count += 1
                    record = future_to_record[future]
                    status, returncode, error = future.result()
                    record.update(status=status, returncode=returncode, error=error)
                    n_finished, n_failed, n_timed_out = report_completion(
                        record, completed_count, len(manifest["runs"]),
                        n_finished, n_failed, n_timed_out)
                    write_manifests(output_dir, manifest)
                    
    return n_finished, n_failed, n_timed_out


def print_summary(manifest: dict[str, Any], n_finished: int, n_failed: int, n_timed_out: int) -> None:
    print(f"Completed {len(manifest['runs'])} simulations: {n_finished} finished, {n_failed} failed, {n_timed_out} timed out.")


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
    if args.resume:
        if args.output_dir:
            output_dir = Path(args.output_dir)
        else:
            # Default to the latest existing run directory
            candidates = sorted(
                [d for d in SIM_ROOT.iterdir() if d.is_dir() and (d / "run_manifest.json").exists()]
            ) if SIM_ROOT.is_dir() else []
            if not candidates:
                print("No previous run directories found to resume.", file=sys.stderr)
                return 1
            output_dir = candidates[-1]
            print(f"Resuming from: {output_dir}")
    else:
        output_dir = Path(args.output_dir) if args.output_dir else SIM_ROOT / timestamp
    output_dir.mkdir(parents=True, exist_ok=True)
    
    try:
        manifest = resolve_runs(
            profiles=profiles,
            experiment_configs=experiment_configs,
            globals_dict=globals_dict,
            output_dir=output_dir,
            profile_dir=profile_dir,
            args=args,
            timestamp=timestamp,
            skipped=skipped,
        )
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
        
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
            
    n_finished, n_failed, n_timed_out = execute_runs(manifest, output_dir, args)
    
    print_summary(manifest, n_finished, n_failed, n_timed_out)
    return 1 if n_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
