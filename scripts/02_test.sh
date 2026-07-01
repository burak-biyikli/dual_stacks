#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ROOT_DIR="$(realpath "$SCRIPT_DIR/..")"
EXT_DIR="$ROOT_DIR/ext"

echo "=== 0. Compiling and Running Integration Tests ==="
ANALYZER_DIR="$ROOT_DIR/tools/trace_analyzer"
DRRUN="$EXT_DIR/dynamorio/build/bin64/drrun"
CLIENT="$ANALYZER_DIR/build/libanalyzer.so"

gcc -O0 -g -pthread $ANALYZER_DIR/tests/test_private.c -o $ANALYZER_DIR/tests/test_private
gcc -O0 -g -pthread $ANALYZER_DIR/tests/test_ipc.c -o $ANALYZER_DIR/tests/test_ipc
gcc -O0 -g $ANALYZER_DIR/tests/test_short_lifetime.c -o $ANALYZER_DIR/tests/test_short_lifetime
gcc -O0 -g $ANALYZER_DIR/tests/test_long_lifetime.c -o $ANALYZER_DIR/tests/test_long_lifetime

echo "Running test_private..."
OUTPUT_PRIVATE=$($DRRUN -c $CLIENT -- $ANALYZER_DIR/tests/test_private 2>&1)
echo "$OUTPUT_PRIVATE"
if echo "$OUTPUT_PRIVATE" | grep -q -E "Strictly IPC \(Instance Level\): +0"; then
    echo "test_private PASSED (0 IPC detected)"
else
    echo "test_private FAILED (IPC unexpectedly detected)"
    exit 1
fi

echo "Running test_ipc..."
OUTPUT_IPC=$($DRRUN -c $CLIENT -- $ANALYZER_DIR/tests/test_ipc 2>&1)
echo "$OUTPUT_IPC"
if echo "$OUTPUT_IPC" | grep -q -E "Strictly IPC \(Instance Level\): +[1-9][0-9]*"; then
    echo "test_ipc PASSED (IPC successfully detected)"
else
    echo "test_ipc FAILED (IPC not detected)"
    exit 1
fi
echo "Running test_short_lifetime..."
OUTPUT_SHORT=$($DRRUN -c $CLIENT -- $ANALYZER_DIR/tests/test_short_lifetime 2>&1)
echo "$OUTPUT_SHORT"

echo "Running test_long_lifetime..."
OUTPUT_LONG=$($DRRUN -c $CLIENT -- $ANALYZER_DIR/tests/test_long_lifetime 2>&1)
echo "$OUTPUT_LONG"
if echo "$OUTPUT_LONG" | grep -q "\[> 2047\]: [1-9]"; then
    echo "test_long_lifetime PASSED (Long lifetime successfully detected)"
else
    echo "test_long_lifetime FAILED (No long lifetime detected)"
    exit 1
fi

echo "=== Success! All integration tests passed! ==="
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
