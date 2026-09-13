#!/usr/bin/env bash
# tools/netsim/jitter.sh — TASK-038 Netsim：抖动
# tc netem delay 注入延迟 + 抖动，验证时序敏感逻辑（移动插值 / 心跳超时）稳定性。
# 用法：bash tools/netsim/jitter.sh --iface eth0 --delay 50ms --jitter 20ms --yes
set -euo pipefail
IFACE=""; SVC=""; YES=""; TEARDOWN=0; DELAY="50ms"; JIT="20ms"
while [ $# -gt 0 ]; do case "$1" in
  --iface) shift; IFACE="$1";; --service) shift; SVC="$1";; --delay) shift; DELAY="$1";; --jitter) shift; JIT="$1";;
  --teardown) TEARDOWN=1;; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1">&2; exit 2;; esac; shift; done
CMD="tc qdisc add dev ${IFACE:-<iface>} root netem delay $DELAY $JIT"
RM="tc qdisc del dev ${IFACE:-<iface>} root"
if [ -n "$SVC" ]; then CMD="docker compose exec $SVC tc qdisc add dev eth0 root netem delay $DELAY $JIT"; RM="docker compose exec $SVC tc qdisc del dev eth0 root"; fi
if [ "$TEARDOWN" = "1" ]; then echo "TEARDOWN: $RM"; eval "$RM" 2>/dev/null || true; exit 0; fi
echo "== netsim: 抖动 ${DELAY}±${JIT} =="; [ -z "$YES" ] && { echo "[dry-run] $CMD"; exit 0; }
eval "$CMD"; echo "已注入；测试后执行: bash $0 --iface ${IFACE:-$SVC} --teardown"
exit 0
