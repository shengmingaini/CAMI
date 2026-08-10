#!/bin/bash
# ================================================================
# CAMI TCP Echo Benchmark Runner
#
# Builds and runs the TCP echo server + benchmark client.
# Tests multiple configurations to find optimal QPS.
#
# Target: QPS >= 100,000
#
# Prerequisites:
#   - CMake 3.20+
#   - C++ compiler with C++17 support (g++/clang++/MSVC)
#   - vcpkg with boost-asio installed
#   - VCPKG_ROOT environment variable set
#
# Usage:
#   bash scripts/run_benchmark.sh
# ================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "=============================================="
echo "  CAMI TCP Echo Benchmark Runner"
echo "=============================================="
echo ""

# === Step 1: Build ===
echo "[1/3] Building..."
if [ ! -d "$BUILD_DIR" ]; then
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        2>&1
else
    cd "$BUILD_DIR"
fi
cmake --build . --target tcp_echo_server tcp_echo_benchmark -j$(nproc 2>/dev/null || echo 4) 2>&1
echo ""

SERVER_BIN="$BUILD_DIR/bin/tcp_echo_server"
CLIENT_BIN="$BUILD_DIR/bin/tcp_echo_benchmark"

# === Step 2: Start Server ===
echo "[2/3] Starting echo server..."
PORT=9999
THREADS=$(nproc 2>/dev/null || echo 4)
$SERVER_BIN $PORT $THREADS &
SERVER_PID=$!
sleep 2
echo "  Server PID: $SERVER_PID"
echo ""

# === Step 3: Run Benchmarks ===
echo "[3/3] Running benchmarks..."
echo ""

# Configuration matrix: connections x msg_size x pipeline
CONFIGS=(
    # "connections msg_size total_msgs pipeline"
    "4   64   500000  4"
    "8   64   500000  4"
    "16  64   1000000 8"
    "32  64   1000000 8"
    "16  128  1000000 8"
    "16  256  1000000 16"
    "32  64   2000000 16"
    "64  64   2000000 16"
)

BEST_QPS=0
echo "+--------+---------+---------+----------+----------+-----------+--------+"
echo "| Conn   | MsgSize | Total   | Pipeline | Time(ms) | QPS       | Result |"
echo "+--------+---------+---------+----------+----------+-----------+--------+"

for config in "${CONFIGS[@]}"; do
    read -r CONN SIZE TOTAL PIPE <<< "$config"
    
    # Run benchmark
    OUTPUT=$($CLIENT_BIN 127.0.0.1 $PORT $CONN $SIZE $TOTAL $PIPE 2>&1)
    
    # Extract results
    QPS=$(echo "$OUTPUT" | grep "Average QPS" | grep -oE '[0-9]+' | tail -1)
    TIME_MS=$(echo "$OUTPUT" | grep "Total time" | grep -oE '[0-9]+' | head -1)
    
    if [ -z "$QPS" ]; then QPS=0; fi
    if [ -z "$TIME_MS" ]; then TIME_MS=0; fi
    
    if [ "$QPS" -gt "$BEST_QPS" ]; then
        BEST_QPS=$QPS
    fi
    
    if [ "$QPS" -ge 100000 ]; then
        RESULT="PASS"
    else
        RESULT="FAIL"
    fi
    
    printf "| %-6s | %-7s | %-7s | %-8s | %-8s | %-9s | %-6s |\n" \
        "$CONN" "${SIZE}B" "$TOTAL" "$PIPE" "$TIME_MS" "$QPS" "$RESULT"
done

echo "+--------+---------+---------+----------+----------+-----------+--------+"
echo ""
echo "  Best QPS: $BEST_QPS"
if [ "$BEST_QPS" -ge 100000 ]; then
    echo "  >>> PASS — Target QPS >= 100,000 achieved <<<"
else
    echo "  >>> FAIL — Target QPS >= 100,000 NOT achieved <<<"
    echo "  Suggestions:"
    echo "    - Increase connections or pipeline depth"
    echo "    - Check CPU core count and thread configuration"
    echo "    - Verify TCP buffer sizes and network stack tuning"
fi
echo ""

# === Cleanup ===
kill $SERVER_PID 2>/dev/null || true
wait $SERVER_PID 2>/dev/null || true
echo "Server stopped. Benchmark complete."
