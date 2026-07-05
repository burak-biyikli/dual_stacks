#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ROOT_DIR="$(realpath "$SCRIPT_DIR/..")"
EXT_DIR="$ROOT_DIR/ext"

echo "=== 0. Running Trace Analyzer Tests ==="
ANALYZER_DIR="$ROOT_DIR/tools/trace_analyzer"
DRRUN="$EXT_DIR/dynamorio/build/bin64/drrun"
CLIENT="$ANALYZER_DIR/build/libanalyzer.so"

"$ANALYZER_DIR/tests/run_tests.sh"
echo ""

echo "=== 1. Testing DynamoRIO Custom Analyzer on 'ls' ==="

if [ -f "$DRRUN" ] && [ -f "$CLIENT" ]; then
    $DRRUN -c "$CLIENT" -- ls -la
else
    echo "DynamoRIO or Client not built yet! Run 01_setup.sh first."
fi

echo "=== 2. Testing GAPBS Natively ==="
BFS_BIN="$EXT_DIR/gapbs/bfs"
if [ -f "$BFS_BIN" ]; then
    # Run BFS on a small synthesized graph of 2^10 nodes for 1 iteration
    $BFS_BIN -g 10 -n 1
else
    echo "GAPBS bfs not built yet! Run 01_setup.sh first."
fi

echo "=== 3. Testing DynamoRIO on GAPBS ==="
if [ -f "$DRRUN" ] && [ -f "$CLIENT" ] && [ -f "$BFS_BIN" ]; then
    $DRRUN -c "$CLIENT" -- $BFS_BIN -g 10 -n 1
else
    echo "Skipping DR+GAPBS test due to missing binaries."
fi

echo "=== 4. Testing gem5 SE mode ==="
GEM5_BIN="$EXT_DIR/gem5/build/X86/gem5.opt"
OUT_DIR="$ROOT_DIR/results/m5out_test"
if [ -f "$GEM5_BIN" ]; then
    $GEM5_BIN --outdir=$OUT_DIR $EXT_DIR/gem5/configs/learning_gem5/part1/simple.py
else
    echo "gem5 binary not found! Did it build successfully?"
fi

echo "=== 5. Testing PARSEC (Blackscholes) ==="
# Build blackscholes
cd "$EXT_DIR/parsec-benchmark"
./bin/parsecmgmt -a build -p blackscholes -c gcc

echo "Running Blackscholes natively (simsmall)..."
./bin/parsecmgmt -a run -p blackscholes -c gcc -i simsmall

echo "Running Blackscholes with DynamoRIO (simsmall)..."
# parsecmgmt -s allows passing a wrapper like DR. We wrap it in quotes.
./bin/parsecmgmt -a run -p blackscholes -c gcc -i simsmall -s "$DRRUN -c $CLIENT --"

echo "=== All Tests Complete ==="
