#!/bin/bash
set -e

# --- Argument Parsing ---
VERBOSE=0
if [[ "$1" == "-v" || "$1" == "--verbose" ]]; then
    VERBOSE=1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ROOT_DIR="$(realpath "$SCRIPT_DIR/../..")"
EXT_DIR="$ROOT_DIR/ext"
GEM5_DIR="$EXT_DIR/gem5"
GEM5_BIN="$GEM5_DIR/build/X86/gem5.opt"

echo "=== Running gem5 Value Predictor Unit Tests ==="
cd "$GEM5_DIR"
UNIT_BUILD_LOG="/tmp/gem5_unit_build.log"

# Build Unit Tests quietly unless verbose
if [ "$VERBOSE" -eq 1 ]; then
    scons build/X86/cpu/valuepred/memory_renaming.test.opt -j$(nproc) --linker=mold --ignore-style USE_CCACHE=1
else
    if ! scons build/X86/cpu/valuepred/memory_renaming.test.opt -j$(nproc) --linker=mold --ignore-style USE_CCACHE=1 > "$UNIT_BUILD_LOG" 2>&1; then
        echo "ERROR: Unit test build failed! See: $UNIT_BUILD_LOG"
        cat "$UNIT_BUILD_LOG"
        exit 1
    fi
fi

# Let the gtest output print directly to the screen
./build/X86/cpu/valuepred/memory_renaming.test.opt


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
                trace_mr="$out_dir/trace_mr.txt"
                clean_stock="$out_dir/trace_stock_clean.txt"
                clean_mr="$out_dir/trace_mr_clean.txt"

                # 2. Run Simulations
                set +e
                "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_stock.txt --stats-file=stats_stock.txt --outdir="$out_dir" "$ROOT_DIR/configs/run_o3_stock.py" "$bin_file" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                stock_status=$?
                
                "$GEM5_BIN" --debug-flags=Exec --debug-file=trace_mr.txt --stats-file=stats_mr.txt --outdir="$out_dir" "$ROOT_DIR/configs/run_o3_mr.py" "$bin_file" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                mr_status=$?
                set -e
                
                if [ $stock_status -ne 0 ] || [ $mr_status -ne 0 ]; then
                    echo "    [FAIL] Simulation CRASHED (Stock exit: $stock_status, MR exit: $mr_status). See:"
                    echo "    stderr: $STDERR_LOG"
                    echo "    stdout: $STDOUT_LOG"
                    FAILURES=$((FAILURES + 1))
                    continue
                fi
                
                # 3. Check Stats
                stats_failed=0
                "$SCRIPT_DIR/check_stats.py" "stock" "$test_name" "$out_dir/stats_stock.txt" || stats_failed=1
                "$SCRIPT_DIR/check_stats.py" "mr" "$test_name" "$out_dir/stats_mr.txt" || stats_failed=1
                
                if [ $stats_failed -eq 1 ]; then
                    FAILURES=$((FAILURES + 1))
                    continue
                fi
                
                # 4. Parse & Diff Traces
                "$SCRIPT_DIR/parse_traces.py" "$trace_stock" "$clean_stock" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                "$SCRIPT_DIR/parse_traces.py" "$trace_mr" "$clean_mr" >> "$STDOUT_LOG" 2>> "$STDERR_LOG"
                
                if diff "$clean_stock" "$clean_mr" > "$out_dir/trace_diff.txt"; then
                    if [ "$VERBOSE" -eq 1 ]; then
                        echo "    [PASS] Traces match perfectly!"
                    fi
                else
                    echo "    [FAIL] Traces diverged! See:"
                    echo "    stderr: $STDERR_LOG"
                    echo "    stdout: $STDOUT_LOG"
                    echo "    divergence diff: $out_dir/trace_diff.txt"
                    FAILURES=$((FAILURES + 1))
                fi
            fi
        done
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