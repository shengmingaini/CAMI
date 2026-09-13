#!/usr/bin/env bash
# tools/chaos/cpu_saturation.sh — TASK-038 Chaos：CPU 饱和
#
# 在目标节点注入 CPU 饱和（stress-ng），验证 20Hz Tick 在压力下仍满足
# P95 < 5ms / P99 < 8ms（§23 性能目标），或至少不发生雪崩式退化。
#
# 安全边界：默认 dry-run；--yes 才注入。注入窗口由 --duration 控制，到期自动停。
#
# 用法：
#   bash tools/chaos/cpu_saturation.sh --service gamenode --yes
#   bash tools/chaos/cpu_saturation.sh --host 10.0.0.7 --cpus 4 --yes

set -euo pipefail
SVC=""; HOST=""; YES=0; CPUS=4; DURATION=30
while [ $# -gt 0 ]; do case "$1" in
  --service) shift; SVC="$1";; --host) shift; HOST="$1";; --cpus) shift; CPUS="$1";;
  --duration) shift; DURATION="$1";; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1" >&2; exit 2;; esac; shift; done

INJECT="stress-ng --cpu $CPUS --timeout ${DURATION}s"
if [ -n "$SVC" ]; then
  echo "== chaos: $SVC 注入 CPU 饱和 ($CPUS 核 / ${DURATION}s) =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  docker compose exec -d "$SVC" $INJECT
elif [ -n "$HOST" ]; then
  echo "== chaos: $HOST 注入 CPU 饱和 =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  ssh "$HOST" "nohup $INJECT >/dev/null 2>&1 &"
else
  echo "必须指定 --service 或 --host" >&2; exit 4
fi
[ "$YES" = "1" ] && echo "注入中（${DURATION}s 后自动停止）。观察 bot_bench tick 百分位是否退化。"
exit 0
