#!/usr/bin/env bash
# 网关带宽基准运行脚本（周四交付物）
# 构建并运行 gateway_bandwidth_bench，把结果 tee 到标准输出与日志。
# 用法: bash scripts/benchmark/gateway_bandwidth.sh
set -e

# 接上本机 MSYS2 工具链（沙箱/CI 之外，本地 MINGW64 用）。
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD=build_bench
cmake -B "$BUILD" -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_BENCHMARK=ON -G "MinGW Makefiles" >/dev/null
cmake --build "$BUILD" --target gateway_bandwidth_bench -j

BIN="$BUILD/benchmark/gateway_bandwidth_bench"
if [ -x "$BIN" ]; then BIN="$BIN"; else BIN="$BIN.exe"; fi
"$BIN"
