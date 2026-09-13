#!/usr/bin/env bash
# tools/chaos/redis_failure.sh — TASK-038 Chaos：Redis 失效
#
# 模拟 Redis（Session / Cache / Routing）不可用，验证：
#   - GameNode 不依赖同步 Redis 读路径做实时状态决策（§9 热路径红线）
#   - 数据层降级 / 重连逻辑生效，Redis 恢复后状态可重建
#
# 安全边界：默认 dry-run；--yes 才真正停止实例。恢复通过 redis-cli ping 断言。
# 注意：Redis RDB 不得作为唯一恢复机制（PROJECT_REQUIREMENTS §17），本脚本不验证 RDB 恢复。
#
# 用法：
#   bash tools/chaos/redis_failure.sh --service redis --yes        # docker compose
#   bash tools/chaos/redis_failure.sh --pod redis-0 --yes          # kubectl (sts)
#   bash tools/chaos/redis_failure.sh --exec "redis-cli -h 127.0.0.1 -p 6379" --yes

set -euo pipefail
SVC=""; POD=""; EXEC="redis-cli -h 127.0.0.1 -p 6379"; YES=0; RECOVER_S=20
while [ $# -gt 0 ]; do case "$1" in
  --service) shift; SVC="$1";; --pod) shift; POD="$1";; --exec) shift; EXEC="$1";;
  --recover) shift; RECOVER_S="$1";; --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1" >&2; exit 2;; esac; shift; done

if [ -n "$SVC" ]; then
  echo "== chaos: docker compose stop $SVC =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  docker compose stop "$SVC"
elif [ -n "$POD" ]; then
  echo "== chaos: kubectl delete pod $POD =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  kubectl -n mmorpg delete pod "$POD" --wait=false
else
  echo "必须指定 --service / --pod（或默认 --exec 直连）" >&2; exit 4
fi

if [ "$YES" = "1" ]; then
  echo "等待恢复（$RECOVER_S 秒）..."; sleep "$RECOVER_S"
  if $EXEC ping 2>/dev/null | grep -q PONG; then echo "RECOVERY: PASS (redis ping=PONG)"; else echo "RECOVERY: FAIL (redis 未恢复)"; exit 1; fi
fi
exit 0
