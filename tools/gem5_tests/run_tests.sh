#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ROOT_DIR="$(realpath "$SCRIPT_DIR/../..")"
EXT_DIR="$ROOT_DIR/ext"
GEM5_DIR="$EXT_DIR/gem5"
GEM5_BIN="$GEM5_DIR/build/X86/gem5.opt"

echo "=== Running gem5 Value Predictor Unit Tests ==="
cd "$GEM5_DIR"
scons build/X86/cpu/valuepred/memory_renaming.test.opt -j$(nproc) --linker=mold --ignore-style USE_CCACHE=1
./build/X86/cpu/valuepred/memory_renaming.test.opt

echo "=== Running gem5 Microbenchmarks ==="
cd "$SCRIPT_DIR"

if [ ! -f "$GEM5_BIN" ]; then
    echo "gem5 binary not found! Did it build successfully?"
    exit 1
fi

for test_dir in */; do
    if [ -d "$test_dir" ]; then
        test_name=$(basename "$test_dir")
        echo "--> Testing $test_name"
        
        # Look for .S files to compile
        for asm_file in "$test_dir"*.S; do
            if [ -f "$asm_file" ]; then
                bin_file="${asm_file%.S}"
                echo "    Compiling $asm_file -> $bin_file"
                gcc -nostdlib "$asm_file" -o "$bin_file"
                
                out_dir="$ROOT_DIR/results/tmp/m5out/$test_name"
                mkdir -p "$out_dir"
                echo "    Running gem5 simulation..."
                # Run but allow failure so one failing test doesn't stop everything (unless we want it to)
                set +e
                $GEM5_BIN --outdir="$out_dir" "$ROOT_DIR/configs/run_o3.py" "$bin_file"
                sim_status=$?
                set -e
                
                if [ $sim_status -eq 0 ]; then
                    echo "    Simulation completed successfully."
                else
                    echo "    Simulation FAILED with exit code $sim_status (likely segfault)."
                fi
            fi
        done
    fi
done

echo "=== All gem5 tests completed ==="
