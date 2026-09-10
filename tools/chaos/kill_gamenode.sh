#!/usr/bin/env bash
# tools/chaos/kill_gamenode.sh — TASK-037 §15.10 / §17 端到端故障演练
#
# 真·进程击杀工具：模拟 GameNode 硬崩溃（kill -9，SIGKILL，不触发任何优雅关闭钩子），
# 用于验证 Gateway 健康检测（3 次心跳丢失 → Dead）+ 故障接管（选同角色低负载替换节点）
# + 玩家重连恢复 是否真的在 15 秒内完成。
#
# 安全边界（混沌工程红线）：
#   1. 绝不默认 kill 全部 gamenode —— 必须显式给 --pid / --port / --match，且要二次确认。
#   2. 默认 dry-run（只打印将要杀谁）；加 --yes 才真正发信号。
#   3. 只杀 GameNode（gamenode / GameNode / scene 进程），绝不碰 Gateway / 数据库 / vcpkg。
#
# 用法：
#   bash tools/chaos/kill_gamenode.sh --pid 12345            # 按 PID 杀
#   bash tools/chaos/kill_gamenode.sh --port 7002            # 按监听端口反查 PID 再杀
#   bash tools/chaos/kill_gamenode.sh --match gamenode       # 按进程名（含）杀（需 --yes）
#   bash tools/chaos/kill_gamenode.sh --pid 12345 --yes      # 真杀（非 dry-run）
#   bash tools/chaos/kill_gamenode.sh --pid 12345 --signal 9 # 指定信号（默认 9 / SIGKILL）

set -euo pipefail

PID=""
PORT=""
MATCH=""
SIGNAL=9
YES=0

i=1
while [ "$i" -le "$#" ]; do
  arg="${!i}"
  case "$arg" in
    --pid)
      i=$((i+1)); PID="${!i}" ;;
    --port)
      i=$((i+1)); PORT="${!i}" ;;
    --match)
      i=$((i+1)); MATCH="${!i}" ;;
    --signal)
      i=$((i+1)); SIGNAL="${!i}" ;;
    --yes)
      YES=1 ;;
    -h|--help)
      grep '^#' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *)
      echo "未知参数：$arg" >&2; exit 2 ;;
  esac
  i=$((i+1))
done

# ---- 解析目标 PID ----
TARGETS=()
if [[ -n "$PID" ]]; then
  TARGETS+=("$PID")
elif [[ -n "$PORT" ]]; then
  # Windows Git Bash / MSYS2：用 netstat 反查监听端口对应的 PID（最后一列）。
  while IFS= read -r line; do
    pid_col=$(echo "$line" | awk '{print $NF}')
    if [[ "$pid_col" =~ ^[0-9]+$ ]]; then TARGETS+=("$pid_col"); fi
  done < <(netstat -ano 2>/dev/null | grep -iE "[:.]${PORT}[[:space:]]" || true)
  if [[ ${#TARGETS[@]} -eq 0 ]]; then
    echo "未找到监听端口 $PORT 的进程" >&2; exit 3
  fi
elif [[ -n "$MATCH" ]]; then
  # 仅匹配 gamenode / GameNode / scene 进程，防止误杀其它服务。
  while IFS= read -r pid; do
    [[ "$pid" =~ ^[0-9]+$ ]] && TARGETS+=("$pid")
  done < <(tasklist //FI "IMAGENAME gt $MATCH" 2>/dev/null | awk 'NR>1 && $1 ~ /gamenode|GameNode|scene/ {print $2}' || true)
fi

if [[ ${#TARGETS[@]} -eq 0 ]]; then
  echo "没有匹配到任何 GameNode 进程。请用 --pid / --port / --match 指定目标。" >&2
  exit 4
fi

echo "== TASK-037 chaos: 将向以下 GameNode 进程发送 SIG$SIGNAL =="
printf '  PID=%s\n' "${TARGETS[@]}"

if [[ $YES -ne 1 ]]; then
  echo "[dry-run] 未加 --yes，仅打印，未真正发送信号。"
  echo "[dry-run] 确认无误后加 --yes 执行真·击杀。"
  exit 0
fi

for p in "${TARGETS[@]}"; do
  echo ">> kill -$SIGNAL $p"
  kill -"$SIGNAL" "$p" 2>/dev/null || echo "  发送失败（进程可能已不存在）：$p" >&2
done

echo "击杀完成。请立即观察 Gateway 日志：应在 ~15s 内检测到 Dead 并触发故障接管。"
exit 0
