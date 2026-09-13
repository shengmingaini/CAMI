#!/usr/bin/env bash
# tools/load/run_ladder.sh — TASK-038 CCU 阶梯压测编排
#
# 逐级跑 bot_bench：100 / 500 / 1000 / 5000 / 10000 / 20000 / 50000 CCU，
# 每级产出 bench/load_<N>.txt，并断言 tick_p99_ms <= 8 与 error_rate <= 0.001。
#
# 本地沙箱（sim 模式）仅能稳定跑 100/500/1000（受单机线程数与 MockGateway 连接数限制）；
# 5000+ 级需要真实分布式网关集群，需通过 --gateway <host:port> 提供，否则标记为 PENDING。
#
# 用法：
#   bash tools/load/run_ladder.sh                  # 本地 sim，跑 100/500/1000
#   bash tools/load/run_ladder.sh --full --gateway 10.0.0.5:9001   # 含 5000+ 级
#
# 红线：不推送 Git / 不改数据库 / 不做与压测无关的网络操作。

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BIN=".verify_041/Release/bin/bot_bench"
[ -x "$BIN" ] || BIN="build/Release/bin/bot_bench"
[ -x "$BIN" ] || { echo "bot_bench 未构建，先构建 bot_bench"; exit 1; }

LEVELS=(100 500 1000)
GATEWAY=""
FULL=0
for a in "$@"; do
  case "$a" in
    --full) FULL=1 ;;
    --gateway) GATEWAY="${2:-}"; shift ;;
    --gateway=*) GATEWAY="${a#*=}" ;;
  esac
done
if [ "$FULL" = "1" ] || [ -n "$GATEWAY" ]; then
  LEVELS=(100 500 1000 5000 10000 20000 50000)
fi

DUR=300                 # 每级默认 300s（与验收脚本一致）
SIM_FLAG="--sim"
EXTRA=()
if [ -n "$GATEWAY" ]; then SIM_FLAG=""; EXTRA=(--gateway "$GATEWAY"); fi

mkdir -p bench
PASS=0; FAIL=0; PENDING=0
printf "%-10s %-12s %-12s %-10s %s\n" LEVEL TICK_P99 ERR_RATE VERDICT NOTE
for N in "${LEVELS[@]}"; do
  # 5000+ 在纯 sim 本地模式标记 PENDING（容量报告已说明），除非提供了真实网关
  if [ "$N" -ge 5000 ] && [ -z "$GATEWAY" ]; then
    printf "%-10s %-12s %-12s %-10s %s\n" "$N" "-" "-" "PENDING" "需分布式网关(--gateway)"
    PENDING=$((PENDING+1))
    continue
  fi
  "$BIN" --bots "$N" --duration "$DUR" $SIM_FLAG "${EXTRA[@]}" >/dev/null 2>&1 || true
  F="bench/load_${N}.txt"
  if [ ! -f "$F" ]; then
    printf "%-10s %-12s %-12s %-10s %s\n" "$N" "-" "-" "FAIL" "无输出文件"
    FAIL=$((FAIL+1)); continue
  fi
  P99=$(grep -oE '^tick_p99_ms=[0-9.]+' "$F" | tail -1 | cut -d= -f2)
  ERR=$(grep -oE '^error_rate=[0-9.]+' "$F" | tail -1 | cut -d= -f2)
  VER="FAIL"; NOTE=""
  awk -v p="$P99" 'BEGIN{exit !(p<=8)}' && awk -v e="$ERR" 'BEGIN{exit !(e<=0.001)}' \
    && { VER="PASS"; PASS=$((PASS+1)); } || { VER="FAIL"; FAIL=$((FAIL+1)); }
  printf "%-10s %-12s %-12s %-10s %s\n" "$N" "$P99" "$ERR" "$VER" "$NOTE"
done

echo "---- ladder summary: pass=$PASS fail=$FAIL pending=$PENDING ----"
[ "$FAIL" -eq 0 ] || exit 1
