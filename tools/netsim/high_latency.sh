#!/usr/bin/env bash
# tools/netsim/high_latency.sh — TASK-038 Netsim：高延迟
# 通过 tc netem 在目标接口注入固定延迟，验证 AOI / 增量同步 / 心跳在弱网下不雪崩。
# 用法：bash tools/netsim/high_latency.sh --iface eth0 --yes
#       bash tools/netsim/high_latency.sh --service gamenode --yes   # 容器内注入
# 默认 dry-run；--teardown 移除条件。
set -euo pipefail
IFACE=""; SVC=""; YES=""; TEARDOWN=0; DELAY=200ms
while [ $# -gt 0 ]; do case "$1" in
  --iface) shift; IFACE="$1";; --service) shift; SVC="$1";; --delay) shift; DELAY="$1";;
  --teardown) TEARDOWN=1;; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1">&2; exit 2;; esac; shift; done
CMD="tc qdisc add dev ${IFACE:-<iface>} root netem delay $DELAY"
RM="tc qdisc del dev ${IFACE:-<iface>} root"
if [ -n "$SVC" ]; then CMD="docker compose exec $SVC tc qdisc add dev eth0 root netem delay $DELAY"; RM="docker compose exec $SVC tc qdisc del dev eth0 root"; fi
if [ "$TEARDOWN" = "1" ]; then echo "TEARDOWN: $RM"; eval "$RM" 2>/dev/null || true; exit 0; fi
echo "== netsim: 注入高延迟 $DELAY =="; [ -z "$YES" ] && { echo "[dry-run] $CMD"; exit 0; }
eval "$CMD"; echo "已注入；测试后执行: bash $0 --iface ${IFACE:-$SVC} --teardown"
exit 0
