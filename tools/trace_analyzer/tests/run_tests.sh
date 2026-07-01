#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ANALYZER_DIR="$SCRIPT_DIR/.."
PROJECT_DIR="$ANALYZER_DIR/../.."

echo "=== Building Analyzer and Tests ==="
cd "$ANALYZER_DIR/build"
make -j4

echo "=== Running Unit Tests ==="
./test_analyzer

echo "=== Running Fuzzer Test ==="
./test_fuzzer

echo "=== Running C Integration Tests ==="
cd "$SCRIPT_DIR"
for TEST in test_ipc test_private test_short_lifetime test_long_lifetime; do
    echo "Running $TEST..."
    $PROJECT_DIR/ext/dynamorio/build/bin64/drrun -c $ANALYZER_DIR/build/libanalyzer.so -- ./$TEST 2> >(grep -E 'Provably Private|Possibly IPC' >&2) >/dev/null
done

echo "=== All Tests Completed Successfully ==="
