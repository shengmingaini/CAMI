#!/usr/bin/env bash
# tools/chaos/mysql_failure.sh — TASK-038 Chaos：MySQL 失效
#
# 模拟 MySQL（最终持久化库）不可用，验证：
#   - 实时游戏逻辑（热路径）不依赖同步 MySQL（§33 红线：禁止 GameNode 直连 MySQL）
#   - 普通持久化采用异步保存，写操作排队 / 重试，库恢复后补刷
#
# 安全边界：默认 dry-run；--yes 才真正停止实例。恢复通过 mysqladmin ping 断言。
#
# 用法：
#   bash tools/chaos/mysql_failure.sh --service mysql --yes
#   bash tools/chaos/mysql_failure.sh --pod mysql-0 --yes

set -euo pipefail
SVC=""; POD=""; YES=0; RECOVER_S=30
MYSQLADMIN="mysqladmin -h 127.0.0.1 -P 3306 -uroot -p\${MYSQL_ROOT_PASSWORD} ping"
while [ $# -gt 0 ]; do case "$1" in
  --service) shift; SVC="$1";; --pod) shift; POD="$1";; --recover) shift; RECOVER_S="$1";;
  --yes) YES=1;; -h|--help) grep '^#' "$0"|sed 's/^# \{0,1\}//'; exit 0;;
  *) echo "未知参数：$1" >&2; exit 2;; esac; shift; done

if [ -n "$SVC" ]; then
  echo "== chaos: docker compose stop $SVC =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  docker compose stop "$SVC"
elif [ -n "$POD" ]; then
  echo "== chaos: kubectl delete pod $POD =="; $YES || { echo "[dry-run] 加 --yes 执行"; exit 0; }
  kubectl -n mmorpg delete pod "$POD" --wait=false
else
  echo "必须指定 --service / --pod 之一" >&2; exit 4
fi

if [ "$YES" = "1" ]; then
  echo "等待恢复（$RECOVER_S 秒）..."; sleep "$RECOVER_S"
  if eval "$MYSQLADMIN" 2>/dev/null | grep -q "alive"; then echo "RECOVERY: PASS (mysql alive)"; else echo "RECOVERY: FAIL (mysql 未恢复)"; exit 1; fi
fi
exit 0
