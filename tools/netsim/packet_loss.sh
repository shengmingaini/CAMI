#!/usr/bin/env bash
# tools/netsim/packet_loss.sh — TASK-038 Netsim：丢包
# tc netem loss 注入随机丢包，验证重传 / 幂等 / 重连逻辑。
# 用法：bash tools/netsim/packet_loss.sh --iface eth0 --loss 2% --yes
set -euo pipefail
IFACE=""; SVC=""; YES=""; TEARDOWN=0; LOSS="2%"
while [ $# -gt 0 ]; do case "$1" in
  --iface) shift; IFACE="$1";; --service) shift; SVC="$1";; --loss) shift; LOSS="$1";;
  --teardown) TEARDOWN=1;; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1">&2; exit 2;; esac; shift; done
CMD="tc qdisc add dev ${IFACE:-<iface>} root netem loss $LOSS"
RM="tc qdisc del dev ${IFACE:-<iface>} root"
if [ -n "$SVC" ]; then CMD="docker compose exec $SVC tc qdisc add dev eth0 root netem loss $LOSS"; RM="docker compose exec $SVC tc qdisc del dev eth0 root"; fi
if [ "$TEARDOWN" = "1" ]; then echo "TEARDOWN: $RM"; eval "$RM" 2>/dev/null || true; exit 0; fi
echo "== netsim: 注入丢包 $LOSS =="; [ -z "$YES" ] && { echo "[dry-run] $CMD"; exit 0; }
eval "$CMD"; echo "已注入；测试后执行: bash $0 --iface ${IFACE:-$SVC} --teardown"
exit 0
