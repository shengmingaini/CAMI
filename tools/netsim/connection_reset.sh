#!/usr/bin/env bash
# tools/netsim/connection_reset.sh — TASK-038 Netsim：随机 RST
# 用 iptables 概率性丢弃/RESET 新建连接，验证断线重连与 Session 重建。
# 用法：bash tools/netsim/connection_reset.sh --iface eth0 --yes   # 主机侧
#       bash tools/netsim/connection_reset.sh --service gamenode --yes  # 容器侧
set -euo pipefail
IFACE=""; SVC=""; YES=""; TEARDOWN=0; PORT=9001
while [ $# -gt 0 ]; do case "$1" in
  --iface) shift; IFACE="$1";; --service) shift; SVC="$1";; --port) shift; PORT="$1";;
  --teardown) TEARDOWN=1;; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1">&2; exit 2;; esac; shift; done
# 用 iptables 对到网关端口的包做 10% 概率 DROP（近似 RST 效果；仅新建连接最受影响）
CMD="iptables -A INPUT -p tcp --dport $PORT -m statistic --mode random --probability 0.10 -j DROP"
RM="iptables -D INPUT -p tcp --dport $PORT -m statistic --mode random --probability 0.10 -j DROP"
if [ -n "$SVC" ]; then CMD="docker compose exec $SVC $CMD"; RM="docker compose exec $SVC $RM"; fi
if [ "$TEARDOWN" = "1" ]; then echo "TEARDOWN: $RM"; eval "$RM" 2>/dev/null || true; exit 0; fi
echo "== netsim: 随机 RST/DROP dport=$PORT =="; [ -z "$YES" ] && { echo "[dry-run] $CMD"; exit 0; }
eval "$CMD"; echo "已注入；测试后执行: bash $0 --port $PORT --teardown"
exit 0
