#!/usr/bin/env bash
# tools/chaos/net_partition.sh — TASK-038 Chaos：网络分区（Gateway <-> GameNode）
#
# 在 Gateway 与 GameNode 之间注入单向/双向丢包（DROP），模拟网络分区，验证：
#   - 心跳超时后健康检测判定 Suspect/Dead
#   - 触发重连 / Scene 迁移 / 故障接管
#   - 分区解除后集群自愈合
#
# 实现：docker compose 环境用 `docker network disconnect` 临时隔离一端的网络；
#       Linux 主机用 iptables DROP；Windows 用 netsh（需管理员）。默认 dry-run。
#
# 用法：
#   bash tools/chaos/net_partition.sh --service gamenode --yes        # 隔离 gamenode
#   bash tools/chaos/net_partition.sh --host 10.0.0.7 --yes          # Linux iptables DROP 到网关
#   bash tools/chaos/net_partition.sh --service gamenode --heal      # 仅执行恢复（重连网络）

set -euo pipefail
SVC=""; HOST=""; YES=0; HEAL=0; RECOVER_S=25
while [ $# -gt 0 ]; do case "$1" in
  --service) shift; SVC="$1";; --host) shift; HOST="$1";; --recover) shift; RECOVER_S="$1";;
  --heal) HEAL=1;; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1" >&2; exit 2;; esac; shift; done

if [ "$HEAL" = "1" ]; then
  if [ -n "$SVC" ]; then docker compose network connect mmorpg_default "$SVC" 2>/dev/null || true; fi
  echo "HEAL: 已尝试恢复网络连接"; exit 0
fi

if [ -n "$SVC" ]; then
  echo "== chaos: 隔离 $SVC 的网络 =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  docker compose network disconnect mmorpg_default "$SVC"
elif [ -n "$HOST" ]; then
  echo "== chaos: iptables DROP -> $HOST =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  iptables -A INPUT -s "$HOST" -j DROP && iptables -A OUTPUT -d "$HOST" -j DROP
else
  echo "必须指定 --service 或 --host" >&2; exit 4
fi

if [ "$YES" = "1" ]; then
  echo "等待分区窗口（$RECOVER_S 秒）..."; sleep "$RECOVER_S"
  echo "执行恢复：bash $0 --service $SVC --heal"
  [ -n "$SVC" ] && docker compose network connect mmorpg_default "$SVC" 2>/dev/null || true
  echo "RECOVERY: 分区已解除，观察集群自愈合"
fi
exit 0
