#!/bin/bash
# =================================================================
# Performance Scaling & Lock Contention Benchmark
#
# Runs the test_perf_scale workload with thread counts from 1 up to
# the CPU logical thread count, with and without the DynamoRIO
# stack privacy analyzer client.
# =================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" &> /dev/null && pwd)"
ANALYZER_DIR="$SCRIPT_DIR/.."
PROJECT_DIR="$ANALYZER_DIR/../.."

DRRUN="$PROJECT_DIR/ext/dynamorio/build/bin64/drrun"
CLIENT="$ANALYZER_DIR/build/libanalyzer.so"
TEST_BIN="$SCRIPT_DIR/test_perf_scale"

if [ ! -f "$DRRUN" ] || [ ! -f "$CLIENT" ]; then
    echo "ERROR: DynamoRIO or client not found!"
    echo "  DRRUN=$DRRUN"
    echo "  CLIENT=$CLIENT"
    exit 1
fi

echo "Compiling test_perf_scale.c..."
gcc -O0 -g -pthread "$SCRIPT_DIR/test_perf_scale.c" -o "$TEST_BIN" 2>&1
if [ $? -ne 0 ]; then
    echo "ERROR: Compilation failed!"
    exit 1
fi

NUM_CORES=$(nproc)
echo "System has $NUM_CORES logical processors."
echo "Running scaling sweep..."
echo ""

# Print header
printf "| Threads | Baseline (s) | Tracked (s) | Slowdown Ratio | Lock Contention Overhead |\n"
printf "|---------|--------------|-------------|----------------|--------------------------|\n"

# Run for 1, 2, 4, 8... up to NUM_CORES. Also guarantee we run at 1 and NUM_CORES.
THREADS_LIST="1"
T=2
while [ $T -lt $NUM_CORES ]; do
    THREADS_LIST="$THREADS_LIST $T"
    T=$((T * 2))
done
if [ $NUM_CORES -gt 1 ]; then
    # Append NUM_CORES if not already in the list
    case " $THREADS_LIST " in
        *" $NUM_CORES "*) ;;
        *) THREADS_LIST="$THREADS_LIST $NUM_CORES" ;;
    esac
fi

for THREADS in $THREADS_LIST; do
    # 1. Run Baseline (no DynamoRIO)
    BASE_TIME=$("$TEST_BIN" "$THREADS" 2>/dev/null)
    
    # 2. Run Tracked (under DynamoRIO client)
    # Suppress output of the stack privacy analyzer client to stderr using 2>/dev/null
    TRACK_TIME=$("$DRRUN" -c "$CLIENT" -- "$TEST_BIN" "$THREADS" 2>/dev/null)
    
    # Calculate ratio and overhead
    if [ -n "$BASE_TIME" ] && [ -n "$TRACK_TIME" ]; then
        RATIO=$(awk "BEGIN {print $TRACK_TIME / $BASE_TIME}")
        # Lock overhead compares slowdown of N threads vs slowdown of 1 thread
        # Let's save T1 times to compute relative scaling overhead
        if [ "$THREADS" -eq 1 ]; then
            T1_BASE=$BASE_TIME
            T1_TRACK=$TRACK_TIME
            OVERHEAD="1.00x (Ref)"
        else
            # Under perfect scaling, time remains constant (weak scaling).
            # Overhead is calculated as Tracked(N)/Tracked(1) divided by Baseline(N)/Baseline(1)
            # which isolates lock contention and profiling scale overhead from baseline OS scheduling scaling.
            OVERHEAD=$(awk "BEGIN {print ($TRACK_TIME / $T1_TRACK) / ($BASE_TIME / $T1_BASE)}")
            OVERHEAD=$(printf "%.2fx" "$OVERHEAD")
        fi
        
        printf "| %7d | %12.4f | %11.4f | %13.2fx | %24s |\n" "$THREADS" "$BASE_TIME" "$TRACK_TIME" "$RATIO" "$OVERHEAD"
    else
        printf "| %7d | %12s | %11s | %14s | %24s |\n" "$THREADS" "ERROR" "ERROR" "N/A" "N/A"
    fi
done

echo ""
exit 0
