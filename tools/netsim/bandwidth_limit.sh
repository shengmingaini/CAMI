#!/usr/bin/env bash
# tools/netsim/bandwidth_limit.sh — TASK-038 Netsim：带宽限制
# tc tbf 限速，验证 Delta Update / 压缩在窄带下仍可用。
# 用法：bash tools/netsim/bandwidth_limit.sh --iface eth0 --rate 1mbit --yes
set -euo pipefail
IFACE=""; SVC=""; YES=""; TEARDOWN=0; RATE="1mbit"
while [ $# -gt 0 ]; do case "$1" in
  --iface) shift; IFACE="$1";; --service) shift; SVC="$1";; --rate) shift; RATE="$1";;
  --teardown) TEARDOWN=1;; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1">&2; exit 2;; esac; shift; done
CMD="tc qdisc add dev ${IFACE:-<iface>} root tbf rate $RATE burst 32kbit latency 400ms"
RM="tc qdisc del dev ${IFACE:-<iface>} root"
if [ -n "$SVC" ]; then CMD="docker compose exec $SVC tc qdisc add dev eth0 root tbf rate $RATE burst 32kbit latency 400ms"; RM="docker compose exec $SVC tc qdisc del dev eth0 root"; fi
if [ "$TEARDOWN" = "1" ]; then echo "TEARDOWN: $RM"; eval "$RM" 2>/dev/null || true; exit 0; fi
echo "== netsim: 限速 $RATE =="; [ -z "$YES" ] && { echo "[dry-run] $CMD"; exit 0; }
eval "$CMD"; echo "已注入；测试后执行: bash $0 --iface ${IFACE:-$SVC} --teardown"
exit 0
