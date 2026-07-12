#!/bin/bash
set -e

# --- Argument Parsing ---
VERBOSE=0
QUICK=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -q|--quick)
            QUICK=1
            shift
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [-v|--verbose] [-q|--quick]"
            exit 1
            ;;
    esac
done

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ROOT_DIR="$(realpath "$SCRIPT_DIR/../..")"
EXT_DIR="$ROOT_DIR/ext"
GEM5_DIR="$EXT_DIR/gem5"
GEM5_BIN="$GEM5_DIR/build/X86/gem5.opt"


cd "$GEM5_DIR"
BUILD_LOG="/tmp/gem5_build.log"

echo -e "\n=== Rebuilding gem5 and unit tests==="

# Define all unit test and simulator targets
TARGETS="build/X86/cpu/valuepred/memory_renaming.test.opt \
         build/X86/cpu/valuepred/memory_renaming_rename_pipeline.test.opt \
         build/X86/cpu/valuepred/memory_renaming_commit_pipeline.test.opt \
         build/X86/gem5.opt"

# Build all targets quietly unless verbose
if [ "$VERBOSE" -eq 1 ]; then
    scons $TARGETS -j$(nproc) --linker=mold --ignore-style USE_CCACHE=1
else
    if ! scons $TARGETS -j$(nproc) --linker=mold --ignore-style USE_CCACHE=1 > "$BUILD_LOG" 2>&1; then
        echo "ERROR: gem5 build failed! See: $BUILD_LOG"
        cat "$BUILD_LOG"
        exit 1
    else
        echo "Build complete"
    fi
fi

echo -e "\n=== Running gem5 Value Predictor Unit Tests ==="

UNIT_TESTS=(
    "build/X86/cpu/valuepred/memory_renaming.test.opt"
    "build/X86/cpu/valuepred/memory_renaming_rename_pipeline.test.opt"
    "build/X86/cpu/valuepred/memory_renaming_commit_pipeline.test.opt"
)

for test_bin in "${UNIT_TESTS[@]}"; do
    echo "--> $(basename "$test_bin")"
    ./"$test_bin"
done

echo -e "\n=== Running gem5 Microbenchmarks ==="
cd "$SCRIPT_DIR"

if [ ! -f "$GEM5_BIN" ]; then
    echo "gem5 binary not found! Did it build successfully?"
    exit 1
fi

FAILURES=0

for test_dir in */; do
    if [ -d "$test_dir" ]; then
        test_name=$(basename "$test_dir")
        echo "--> Testing $test_name"
        
        for asm_file in "$test_dir"*.S; do
            if [ -f "$asm_file" ]; then
                bin_file="${asm_file%.S}"
                out_dir="/tmp/gem5_dual_stacks_tests/$test_name"
                mkdir -p "$out_dir"
                
                STDOUT_LOG="$out_dir/stdout.txt"
                STDERR_LOG="$out_dir/stderr.txt"
                
                # Clear logs from previous runs
                > "$STDOUT_LOG"
                > "$STDERR_LOG"
                
                # 1. Compile
                if ! gcc -nostdlib "$asm_file" -o "$bin_file" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"; then
                    echo "    [FAIL] Compilation failed! See:"
                    echo "    stderr: $STDERR_LOG"
                    echo "    stdout: $STDOUT_LOG"
                    FAILURES=$((FAILURES + 1))
                    continue
                fi
                
                trace_stock="$out_dir/trace_stock.txt"
                trace_mr_penalty="$out_dir/trace_mr_penalty.txt"
                trace_mr_reinit="$out_dir/trace_mr_reinit.txt"
                clean_stock="$out_dir/trace_stock_clean.txt"
                clean_mr_penalty="$out_dir/trace_mr_penalty_clean.txt"
                clean_mr_reinit="$out_dir/trace_mr_reinit_clean.txt"

                # 2. Run Simulations
                set +e
                "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_stock.txt --stats-file=stats_stock.txt --outdir="$out_dir" "$ROOT_DIR/configs/run_o3_stock.py" "$bin_file" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                stock_status=$?
                
                "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_mr_penalty.txt --stats-file=stats_mr_penalty.txt --outdir="$out_dir" "$ROOT_DIR/configs/run_o3_mr.py" "$bin_file" --realloc-mode penalty --realloc-amount 1 >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                mr_pen_status=$?

                "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_mr_reinit.txt --stats-file=stats_mr_reinit.txt --outdir="$out_dir" "$ROOT_DIR/configs/run_o3_mr.py" "$bin_file" --realloc-mode reinit --realloc-amount 1 >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                mr_reinit_status=$?
                set -e
                
                if [ $stock_status -ne 0 ] || [ $mr_pen_status -ne 0 ] || [ $mr_reinit_status -ne 0 ]; then
                    echo "    [FAIL] Simulation CRASHED (Stock exit: $stock_status, MR Penalty exit: $mr_pen_status, MR Reinit exit: $mr_reinit_status). See:"
                    echo "    stderr: $STDERR_LOG"
                    echo "    stdout: $STDOUT_LOG"
                    FAILURES=$((FAILURES + 1))
                    continue
                fi
                
                # 3. Check Stats
                stats_failed=0
                "$SCRIPT_DIR/check_stats.py" "stock" "$test_name" "$out_dir/stats_stock.txt" || stats_failed=1
                "$SCRIPT_DIR/check_stats.py" "mr_penalty" "$test_name" "$out_dir/stats_mr_penalty.txt" "$out_dir/stats_stock.txt" || stats_failed=1
                "$SCRIPT_DIR/check_stats.py" "mr_reinit" "$test_name" "$out_dir/stats_mr_reinit.txt" "$out_dir/stats_stock.txt" || stats_failed=1
                
                if [ $stats_failed -eq 1 ]; then
                    FAILURES=$((FAILURES + 1))
                    continue
                fi
                
                # 4. Parse & Diff Traces
                "$SCRIPT_DIR/parse_traces.py" "$trace_stock" "$clean_stock" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                "$SCRIPT_DIR/parse_traces.py" "$trace_mr_penalty" "$clean_mr_penalty" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                "$SCRIPT_DIR/parse_traces.py" "$trace_mr_reinit" "$clean_mr_reinit" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                
                penalty_diff_failed=0
                reinit_diff_failed=0
                
                if diff "$clean_stock" "$clean_mr_penalty" > "$out_dir/trace_diff_penalty.txt"; then
                    :
                else
                    penalty_diff_failed=1
                fi
                
                if diff "$clean_stock" "$clean_mr_reinit" > "$out_dir/trace_diff_reinit.txt"; then
                    :
                else
                    reinit_diff_failed=1
                fi
                
                if [ $penalty_diff_failed -eq 0 ] && [ $reinit_diff_failed -eq 0 ]; then
                    if [ "$VERBOSE" -eq 1 ]; then
                        echo "    [PASS] Traces match perfectly!"
                    fi
                else
                    echo "    [FAIL] Traces diverged! See:"
                    echo "    stderr: $STDERR_LOG"
                    echo "    stdout: $STDOUT_LOG"
                    if [ $penalty_diff_failed -eq 1 ]; then
                        echo "    divergence diff (penalty): $out_dir/trace_diff_penalty.txt"
                    fi
                    if [ $reinit_diff_failed -eq 1 ]; then
                        echo "    divergence diff (reinit): $out_dir/trace_diff_reinit.txt"
                    fi
                    FAILURES=$((FAILURES + 1))
                fi
            fi
        done
    fi
done

if [ "$QUICK" -eq 1 ]; then
    echo -e "\n=== Skipping Macrobenchmarks (Quick Mode) ==="
    if [ $FAILURES -gt 0 ]; then
        echo -e "\n\n==========================================="
        echo "Test Summary: FAILED. $FAILURES test(s) failed."
        echo "==========================================="
        exit 1
    else
        echo -e "\n\n==========================================="
        echo "Test Summary: SUCCESS. All tests passed."
        echo "==========================================="
        exit 0
    fi
fi

# --- Macrobenchmark Hybrid Testing Loop ---
echo -e "\n=== Running gem5 Macrobenchmarks (Hybrid Mode) ==="

# Ensure OMP_NUM_THREADS=1 is exported so OpenMP doesn't spawn excess threads
export OMP_NUM_THREADS=1

bench_names=("bfs" "blackscholes")
bench_bins=("$ROOT_DIR/ext/gapbs/bfs" "$ROOT_DIR/ext/parsec-benchmark/pkgs/apps/blackscholes/inst/amd64-linux.gcc/bin/blackscholes")

for i in "${!bench_names[@]}"; do
    name="${bench_names[$i]}"
    bin="${bench_bins[$i]}"
    
    if [ ! -f "$bin" ]; then
        echo "Benchmark binary $bin not found! Skipping..."
        continue
    fi
    
    echo "--> Running Hybrid Test for $name"
    
    # Get arguments
    case "$name" in
        "bfs")
            args=("-g" "10" "-n" "1")
            ;;
        "blackscholes")
            args=("1" "$ROOT_DIR/ext/parsec-benchmark/pkgs/apps/blackscholes/inputs/simsmall/in_4K.txt" "/tmp/prices.txt")
            ;;
        *)
            args=()
            ;;
    esac
    
    out_dir="/tmp/gem5_dual_stacks_tests/$name"
    mkdir -p "$out_dir"
    
    STDOUT_LOG="$out_dir/stdout.txt"
    STDERR_LOG="$out_dir/stderr.txt"
    > "$STDOUT_LOG"
    > "$STDERR_LOG"
    
    trace_stock="$out_dir/trace_stock.txt"
    trace_mr="$out_dir/trace_mr.txt"
    clean_stock="$out_dir/trace_stock_clean.txt"
    clean_mr="$out_dir/trace_mr_clean.txt"
    
    # ----------------------------------------------------
    # Phase 1: Early Pipeline Integrity (Trace Mode)
    # ----------------------------------------------------
    if [ "$VERBOSE" -eq 1 ]; then
        echo "    [Phase 1] Trace Mode (100K instructions)..."
    fi
    set +e
    "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_stock.txt --stats-file=stats_stock.txt --outdir="$out_dir" \
        "$ROOT_DIR/configs/run_o3_stock.py" "$bin" --max-insts 100000 "${args[@]}" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
    stock_status=$?
    
    "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_mr.txt --stats-file=stats_mr.txt --outdir="$out_dir" \
        "$ROOT_DIR/configs/run_o3_mr.py" "$bin" --max-insts 100000 "${args[@]}" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
    mr_status=$?
    set -e
    
    # Run the trace parsing and diffing even if a crash occurred, as long as trace files exist
    if [ -f "$trace_stock" ] && [ -f "$trace_mr" ]; then
        set +e
        "$SCRIPT_DIR/parse_traces.py" "$trace_stock" "$clean_stock" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
        "$SCRIPT_DIR/parse_traces.py" "$trace_mr" "$clean_mr" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
        diff "$clean_stock" "$clean_mr" > "$out_dir/trace_diff.txt"
        diff_status=$?
        set -e
    else
        diff_status=0
    fi
    
    if [ $stock_status -ne 0 ] || [ $mr_status -ne 0 ]; then
        echo "    [FAIL] Phase 1 Simulation CRASHED (Stock exit: $stock_status, MR exit: $mr_status). See:"
        echo "    stderr: $STDERR_LOG"
        echo "    stdout: $STDOUT_LOG"
        if [ -f "$out_dir/trace_diff.txt" ] && [ -s "$out_dir/trace_diff.txt" ]; then
            echo "    Trace divergence diff is available at: $out_dir/trace_diff.txt"
        fi
        FAILURES=$((FAILURES + 1))
        continue
    fi
    
    if [ $diff_status -ne 0 ]; then
        echo "    [FAIL] Phase 1 traces diverged! See:"
        echo "    stderr: $STDERR_LOG"
        echo "    stdout: $STDOUT_LOG"
        echo "    divergence diff: $out_dir/trace_diff.txt"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    
    # ----------------------------------------------------
    # Phase 2: Deep Execution Stress (Stress Mode)
    # ----------------------------------------------------
    if [ "$VERBOSE" -eq 1 ]; then
        echo "    [Phase 2] Stress Mode (15M instructions, no tracing)..."
    fi
    set +e
    "$GEM5_BIN" --stats-file=stats_stock_stress.txt --outdir="$out_dir" \
        "$ROOT_DIR/configs/run_o3_stock.py" "$bin" --max-insts 15000000 "${args[@]}" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
    stock_status=$?
    
    "$GEM5_BIN" --stats-file=stats_mr_stress.txt --outdir="$out_dir" \
        "$ROOT_DIR/configs/run_o3_mr.py" "$bin" --max-insts 15000000 "${args[@]}" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
    mr_status=$?
    set -e
    
    if [ $stock_status -ne 0 ] || [ $mr_status -ne 0 ]; then
        echo "    [FAIL] Phase 2 Simulation FAILED (Stock exit: $stock_status, MR exit: $mr_status). See:"
        echo "    stderr: $STDERR_LOG"
        echo "    stdout: $STDOUT_LOG"
        FAILURES=$((FAILURES + 1))
        continue
    fi
    
    if [ "$VERBOSE" -eq 1 ]; then
        echo "    [PASS] Hybrid Test for $name complete (Phases 1 & 2 passed)."
    fi
done


if [ $FAILURES -gt 0 ]; then
    echo -e "\n\n==========================================="
    echo "Test Summary: FAILED. $FAILURES test(s) failed."
    echo "==========================================="
    exit 1
else
    echo -e "\n\n==========================================="
    echo "Test Summary: SUCCESS. All tests passed."
    echo "==========================================="
    exit 0
fi