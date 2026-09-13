#!/usr/bin/env bash
# tools/netsim/flaky_dns.sh — TASK-038 Netsim：DNS 抖动（尽力而为）
# 真实 DNS 故障难以用 tc 模拟；此处用 iptables 对 53 端口做概率 DROP，
# 并在恢复窗口后断言（仅当 --yes 且提供 --probe-host 时）。
# 用法：bash tools/netsim/flaky_dns.sh --yes --probe-host redis
set -euo pipefail
YES=""; TEARDOWN=0; PROBE=""
while [ $# -gt 0 ]; do case "$1" in
  --probe-host) shift; PROBE="$1";; --teardown) TEARDOWN=1;; --yes) YES=1;;
  -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;; *) echo "未知参数：$1">&2; exit 2;; esac; shift; done
CMD="iptables -A OUTPUT -p udp --dport 53 -m statistic --mode random --probability 0.20 -j DROP"
RM="iptables -D OUTPUT -p udp --dport 53 -m statistic --mode random --probability 0.20 -j DROP"
if [ "$TEARDOWN" = "1" ]; then echo "TEARDOWN: $RM"; eval "$RM" 2>/dev/null || true; exit 0; fi
echo "== netsim: DNS 抖动（53/udp 20% DROP）=="; [ -z "$YES" ] && { echo "[dry-run] $CMD"; exit 0; }
eval "$CMD"
if [ -n "$PROBE" ]; then
  echo "断言：恢复窗口内可解析 $PROBE"; getent hosts "$PROBE" >/dev/null 2>&1 \
    && echo "DNS PROBE: PASS" || echo "DNS PROBE: (需服务发现容错) FAIL"
fi
echo "已注入；测试后执行: bash $0 --teardown"
exit 0
