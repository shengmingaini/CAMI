#!/usr/bin/env bash
# ============================================================================
# CAMI 网关压测运行脚本 [PROTOTYPE]
# 在已调优的 Linux 主机上跑"单机 5 万长连接 / 30 分钟"验收。
#
# 用法：
#   bash scripts/benchmark/run-50k-stress.sh \
#       --total 50000 --ports 7910,7911,7912,7913 \
#       --duration 1800000 --build 1
#
# 关键约束（详见 docs/benchmark/stress-test-report-v1.md）：
#   单客户端进程连单一 server(ip:port) 受本机临时端口数限制（Linux 默认 ~2.8 万，
#   调优后 ~6.4 万）。5 万连接须用"多 server 端口 × 多 client 进程"分摊，故默认 4 端口。
#   Windows(MinGW) 仅用于缩量冒烟（单端口 ≤1.6 万连接），全量必须在 Linux 跑。
# ============================================================================
set -u
TOTAL=50000
PORTS="7910,7911,7912,7913"
DURATION=1800000   # 毫秒：30 分钟
BUILD=0
HOST="127.0.0.1"
THREADS=4
HB_TIMEOUT=15000
INTERVAL=5000

while [ $# -gt 0 ]; do
  case "$1" in
    --total)    TOTAL="$2"; shift 2;;
    --ports)    PORTS="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --build)    BUILD="$2"; shift 2;;
    --host)     HOST="$2"; shift 2;;
    --threads)  THREADS="$2"; shift 2;;
    *) echo "unknown arg: $1"; exit 1;;
  esac
done

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

if [ "$BUILD" = "1" ]; then
  echo "[run] configuring + building stress binaries ..."
  cmake -B build -DCAMI_BUILD_MODULES=OFF -DCAMI_BUILD_BENCHMARK=ON -G "$(command -v ninja >/dev/null && echo Ninja || echo 'Unix Makefiles')" >/dev/null 2>&1
  cmake --build build -j"$(nproc)" --target gateway_stress_server gateway_stress_client
fi

IFS=',' read -ra PORT_ARR <<< "$PORTS"
N=${#PORT_ARR[@]}
PER=$(( (TOTAL + N - 1) / N ))   # 每端口分摊连接数（向上取整）

PIDS=()
LOGDIR="build/stress_logs"; mkdir -p "$LOGDIR"

echo "[run] launching $N server + $N client pairs; total≈$((PER*N)) conns; duration=${DURATION}ms"
for p in "${PORT_ARR[@]}"; do
  ./build/bin/gateway_stress_server --host 0.0.0.0 --port "$p" --threads "$THREADS" \
      --hb-timeout "$HB_TIMEOUT" --duration "$((DURATION+60000))" > "$LOGDIR/srv_$p.log" 2>&1 &
  PIDS+=($!)
  sleep 0.3
  ./build/bin/gateway_stress_client --host "$HOST" --port "$p" --connections "$PER" \
      --interval "$INTERVAL" --duration "$DURATION" > "$LOGDIR/cli_$p.log" 2>&1 &
  PIDS+=($!)
done

echo "[run] all launched (pids: ${PIDS[*]}). Press Ctrl-C to abort early."
wait

echo "[run] done. per-port logs in $LOGDIR/"
for p in "${PORT_ARR[@]}"; do
  echo "--- server $p final ---"; tail -n 2 "$LOGDIR/srv_$p.log"
done
