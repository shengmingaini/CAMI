#!/usr/bin/env bash
# tools/netsim/run_netsim.sh — TASK-038 Netsim 编排：六种弱网场景逐项注入 + 压测探针
#
# 每项：注入条件 -> 跑一次短 bot_bench 探针（--bots 100 --duration 20）记录 tick 百分位 ->
#       拆除条件。最终汇总各场景下的退化幅度（不要求达标，仅记录真实影响）。
#
# 用法：
#   bash tools/netsim/run_netsim.sh --dry                 # 仅打印 tc 命令
#   bash tools/netsim/run_netsim.sh --yes --iface eth0    # 主机侧注入
#   bash tools/netsim/run_netsim.sh --yes --service gamenode  # 容器侧注入
#
# 红线：--yes 才注入；每项结束必拆；不持久化任何 qdisc。

set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DIR/../.." && pwd)"
YES=""; TARGET=()
for a in "$@"; do
  case "$a" in
    --yes) YES="--yes" ;; --dry) YES="" ;;
    --iface|--service) TARGET+=("$a" "${2:-}"); shift ;;
    --iface=*|--service=*) TARGET+=("${a%%=*}" "${a#*=}") ;;
    *) echo "忽略未知参数：$a" >&2 ;;
  esac
done
[ ${#TARGET[@]} -eq 0 ] && { echo "需指定 --iface <dev> 或 --service <svc>" >&2; exit 4; }

BIN=".verify_041/Release/bin/bot_bench"; [ -x "$BIN" ] || BIN="build/Release/bin/bot_bench"
PROBE=(--bots 100 --duration 20 --sim)

echo "===== Netsim 六场景 ====="
for s in high_latency packet_loss bandwidth_limit jitter connection_reset flaky_dns; do
  echo "----- $s -----"
  bash "$DIR/$s.sh" "${TARGET[@]}" $YES || true
  if [ -n "$YES" ] && [ -x "$BIN" ]; then
    "$BIN" "${PROBE[@]}" >/dev/null 2>&1 || true
    F="bench/load_100.txt"
    [ -f "$F" ] && echo "  探针 tick_p99_ms=$(grep -oE '^tick_p99_ms=[0-9.]+' "$F"|cut -d= -f2) error_rate=$(grep -oE '^error_rate=[0-9.]+' "$F"|cut -d= -f2)"
  fi
  # 拆除
  bash "$DIR/$s.sh" "${TARGET[@]}" --teardown 2>/dev/null || true
done
echo "===== Netsim 完成（所有条件已拆除）====="
