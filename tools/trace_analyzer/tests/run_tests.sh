#!/bin/bash
# =================================================================
# Trace Analyzer Test Runner
#
# Runs all unit and integration tests for the trace analyzer.
# Reports pass/fail per test and prints a summary at the end.
#
# Usage:
#   ./tests/run_tests.sh          # Run from trace_analyzer directory
#
# Prerequisites:
#   - DynamoRIO built in ext/dynamorio/build
#   - trace_analyzer built in tools/trace_analyzer/build
# =================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ANALYZER_DIR="$SCRIPT_DIR/.."
PROJECT_DIR="$ANALYZER_DIR/../.."

DRRUN="$PROJECT_DIR/ext/dynamorio/build/bin64/drrun"
CLIENT="$ANALYZER_DIR/build/libanalyzer.so"

PASSED=0
FAILED=0
FAILED_TESTS=""

pass() {
    echo "  PASSED: $1"
    PASSED=$((PASSED + 1))
}

fail() {
    echo "  FAILED: $1"
    FAILED=$((FAILED + 1))
    FAILED_TESTS="$FAILED_TESTS  - $1\n"
}

# =================================================================
# Build
# =================================================================
echo "=== Building Analyzer and Tests ==="
cd "$ANALYZER_DIR/build"
if ! make -j$(nproc) 2>&1; then
    echo "FATAL: Build failed!"
    exit 1
fi
echo ""

# =================================================================
# Unit Tests (GTest)
# =================================================================
echo "=== Running Unit Tests (GTest) ==="
if ./test_analyzer --gtest_color=yes 2>&1; then
    pass "Unit Tests (test_analyzer)"
else
    fail "Unit Tests (test_analyzer)"
fi
echo ""

# =================================================================
# Integration Tests (under DynamoRIO)
# =================================================================
echo "=== Running Integration Tests ==="

if [ ! -f "$DRRUN" ] || [ ! -f "$CLIENT" ]; then
    echo "WARNING: DynamoRIO or client not found, skipping integration tests."
    echo "  DRRUN=$DRRUN"
    echo "  CLIENT=$CLIENT"
else
    # --- Compile test programs ---
    echo "Compiling integration test programs..."
    cd "$SCRIPT_DIR"
    gcc -O0 -g -pthread test_private.c -o test_private 2>&1
    gcc -O0 -g -pthread test_ipc.c -o test_ipc 2>&1
    gcc -O0 -g -pthread test_ipc_ghr.c -o test_ipc_ghr 2>&1
    gcc -O0 -g -pthread test_stack_reuse.c -o test_stack_reuse 2>&1
    gcc -O0 -g test_short_lifetime.c -o test_short_lifetime 2>&1
    gcc -O0 -g test_long_lifetime.c -o test_long_lifetime 2>&1
    echo ""

    # --- test_private: All operations should be provably private ---
    echo "Running test_private..."
    OUTPUT=$($DRRUN -c $CLIENT -- ./test_private 2>&1)
    if echo "$OUTPUT" | grep -q -E "Strictly IPC \(Instance Level\): +0"; then
        pass "test_private (0 IPC detected)"
    else
        fail "test_private (expected Strictly IPC = 0)"
        echo "$OUTPUT" | grep -E "Strictly IPC|Provably Private" | head -5
    fi

    # --- test_ipc: Should detect cross-thread IPC ---
    echo "Running test_ipc..."
    OUTPUT=$($DRRUN -c $CLIENT -- ./test_ipc 2>&1)
    if echo "$OUTPUT" | grep -q -E "Strictly IPC \(Instance Level\): +[1-9][0-9]*"; then
        pass "test_ipc (IPC successfully detected)"
    else
        fail "test_ipc (expected Strictly IPC > 0)"
        echo "$OUTPUT" | grep -E "Strictly IPC|Provably Private" | head -5
    fi

    # --- test_ipc_ghr: Verify GHR context bucket refinement reduces Possibly IPC false positives ---
    echo "Running test_ipc_ghr..."
    OUTPUT=$($DRRUN -c $CLIENT -- ./test_ipc_ghr 2>&1)
    NON_HIST=$(echo "$OUTPUT" | grep -oP "Possibly IPC \(Non-History\): +\K[0-9]+")
    HIST=$(echo "$OUTPUT" | grep -oP "Possibly IPC \(History-Based, [0-9]+ buckets\): +\K[0-9]+")
    if [ -n "$NON_HIST" ] && [ -n "$HIST" ] && [ "$NON_HIST" -gt "$HIST" ]; then
        pass "test_ipc_ghr (GHR refinement successfully verified: Non-History ($NON_HIST) > History-Based ($HIST))"
    else
        fail "test_ipc_ghr (expected Possibly IPC Non-History > History-Based)"
        echo "$OUTPUT" | grep -E "Possibly IPC"
    fi

    # --- test_stack_reuse: Thread exit + stack address reuse should have 0 IPC ---
    echo "Running test_stack_reuse..."
    OUTPUT=$($DRRUN -c $CLIENT -- ./test_stack_reuse 2>&1)
    if echo "$OUTPUT" | grep -q -E "Strictly IPC \(Instance Level\): +0"; then
        pass "test_stack_reuse (0 IPC detected)"
    else
        fail "test_stack_reuse (expected Strictly IPC = 0)"
        echo "$OUTPUT" | grep -E "Strictly IPC|Provably Private" | head -5
    fi

    # --- test_short_lifetime: Should have histogram entries in low bins ---
    echo "Running test_short_lifetime..."
    OUTPUT=$($DRRUN -c $CLIENT -- ./test_short_lifetime 2>&1)
    if echo "$OUTPUT" | grep -q "Stack Lifetime Histogram"; then
        pass "test_short_lifetime (histogram present)"
    else
        fail "test_short_lifetime (no histogram output)"
    fi

    # --- test_long_lifetime: Should have entries in the overflow bin ---
    echo "Running test_long_lifetime..."
    OUTPUT=$($DRRUN -c $CLIENT -- ./test_long_lifetime 2>&1)
    if echo "$OUTPUT" | grep -q "\[> 2047\]: [1-9]"; then
        pass "test_long_lifetime (overflow bin populated)"
    else
        fail "test_long_lifetime (expected entries in [> 2047] bin)"
        echo "$OUTPUT" | grep -E "\[> 2047\]|Histogram" | head -5
    fi

    # --- test_fuzzer_minimal_stress: Should detect IPC under heavy churn ---
    echo "Running test_fuzzer_minimal_stress (this may take a moment)..."
    OUTPUT=$($DRRUN -c $CLIENT -- "$ANALYZER_DIR/build/test_fuzzer" 2>&1)
    FUZZER_OK=true

    if echo "$OUTPUT" | grep -q -E "Strictly IPC \(Instance Level\): +[1-9][0-9]*"; then
        : # IPC detected
    else
        FUZZER_OK=false
    fi

    # Check that at least 2 unique PCs have IPC (push PC + load/store PC)
    IPC_PCS=$(echo "$OUTPUT" | grep -oP "Unique Raw Stack PCs with IPC: +\K[0-9]+" | head -1)
    if [ -n "$IPC_PCS" ] && [ "$IPC_PCS" -ge 2 ] 2>/dev/null; then
        : # At least 2 IPC PCs
    else
        FUZZER_OK=false
    fi

    if $FUZZER_OK; then
        pass "test_fuzzer_minimal_stress (IPC detected, >= 2 IPC PCs)"
    else
        fail "test_fuzzer_minimal_stress (expected IPC > 0 and >= 2 IPC PCs)"
        echo "$OUTPUT" | grep -E "Strictly IPC|Unique Raw Stack PCs" | head -5
    fi

    # --- Performance Scaling Sweep ---
    echo "Running Performance Scaling Sweep..."
    if "$SCRIPT_DIR/run_perf_scale.sh"; then
        pass "Performance Scaling Sweep"
    else
        fail "Performance Scaling Sweep (execution error)"
    fi
fi

# =================================================================
# Summary
# =================================================================
TOTAL=$((PASSED + FAILED))

if [ $FAILED -gt 0 ]; then
    echo -e "\n\n==========================================="
    echo "Test Summary: FAILED. $FAILED test(s) failed."
    echo "==========================================="
    exit 1
else
    echo -e "\n\n==========================================="
    echo "Test Summary: SUCCESS. $PASSED/$TOTAL passed."
    echo "==========================================="
    exit 0
fi
