#!/usr/bin/env bash
# tools/chaos/run_chaos.sh — TASK-038 Chaos 编排：七项故障逐项执行 + 恢复断言
#
# 七项：GameNode Crash / Gateway Crash / Redis Failure / MySQL Failure /
#       Network Partition / CPU Saturation / Memory Pressure
# 每项通过对应脚本执行，--yes 真正注入，并在恢复窗口后断言（脚本内 RECOVERY 输出）。
#
# 用法：
#   bash tools/chaos/run_chaos.sh --dry            # 全部 dry-run（默认）
#   bash tools/chaos/run_chaos.sh --yes --service gamenode   # 以 compose 服务为靶
#   bash tools/chaos/run_chaos.sh --yes --pod gamenode-xyz   # 以 k8s pod 为靶
#
# 红线：不在无 --yes 时造成任何破坏性动作；逐项独立、可单独重跑。

set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YES=""
TARGET=()
for a in "$@"; do
  case "$a" in
    --yes) YES="--yes" ;;
    --dry) YES="" ;;
    --service|--pod) TARGET+=("$a" "${2:-}"); shift ;;
    --service=*|--pod=*) TARGET+=("${a%%=*}" "${a#*=}") ;;
    *) echo "忽略未知参数：$a" >&2 ;;
  esac
done
[ ${#TARGET[@]} -eq 0 ] && TARGET=(--service gamenode)

PASS=0; FAIL=0; PENDING=0
run_one() {
  local name="$1"; shift
  echo "----------------------------------------------------------------"
  echo ">> $name"
  if bash "$DIR/$1" "${TARGET[@]}" $YES; then
    [ -z "$YES" ] && PENDING=$((PENDING+1)) || PASS=$((PASS+1))
  else
    FAIL=$((FAIL+1))
  fi
}

run_one "GameNode Crash"        kill_gamenode.sh
run_one "Gateway Crash"         gateway_crash.sh
run_one "Redis Failure"         redis_failure.sh
run_one "MySQL Failure"         mysql_failure.sh
run_one "Network Partition"     net_partition.sh
run_one "CPU Saturation"        cpu_saturation.sh
run_one "Memory Pressure"       memory_pressure.sh

echo "----------------------------------------------------------------"
echo "chaos summary: pass=$PASS fail=$FAIL pending(dry)=$PENDING"
# dry-run 不计入失败；真实注入(--yes)失败则非零退出
[ "$FAIL" -eq 0 ] || exit 1
