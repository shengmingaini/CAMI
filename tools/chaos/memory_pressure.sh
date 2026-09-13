#!/usr/bin/env bash
# tools/chaos/memory_pressure.sh — TASK-038 Chaos：内存压力
#
# 在目标节点注入内存压力（stress-ng --vm），验证 OOM 前的优雅降级与故障恢复，
# 以及 GameNode 在内存回落后能继续服务，不触发全局状态丢失（§10 单一状态 Owner）。
#
# 安全边界：默认 dry-run；--yes 才注入。--vm-bytes 默认 70%（保守，避免直接 OOM kill）。
#
# 用法：
#   bash tools/chaos/memory_pressure.sh --service gamenode --yes
#   bash tools/chaos/memory_pressure.sh --host 10.0.0.7 --vm-bytes 70% --yes

set -euo pipefail
SVC=""; HOST=""; YES=0; VMBYTES="70%"; DURATION=30
while [ $# -gt 0 ]; do case "$1" in
  --service) shift; SVC="$1";; --host) shift; HOST="$1";; --vm-bytes) shift; VMBYTES="$1";;
  --duration) shift; DURATION="$1";; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1" >&2; exit 2;; esac; shift; done

INJECT="stress-ng --vm 1 --vm-bytes $VMBYTES --timeout ${DURATION}s"
if [ -n "$SVC" ]; then
  echo "== chaos: $SVC 注入内存压力 ($VMBYTES / ${DURATION}s) =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  docker compose exec -d "$SVC" $INJECT
elif [ -n "$HOST" ]; then
  echo "== chaos: $HOST 注入内存压力 =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  ssh "$HOST" "nohup $INJECT >/dev/null 2>&1 &"
else
  echo "必须指定 --service 或 --host" >&2; exit 4
fi
[ "$YES" = "1" ] && echo "注入中（${DURATION}s 后自动停止）。观察 GameNode 是否优雅降级 / 恢复。"
exit 0
