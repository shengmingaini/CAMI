#!/bin/bash
# ============================================================================
# CAMI Dev-Mini 起步档 — MySQL 单实例初始化 (Scale-to-Fit)
# ----------------------------------------------------------------------------
# 在 1 个物理 MySQL 实例内创建:
#   - 8 个逻辑分片库  cami_shard_0 .. cami_shard_7   (分片算法 %8 固定, ADR-003)
#   - 1 个全局库      cami_global                    (账号/拍卖行/公会等非分片数据)
# 每个分片库执行 init-shard.sql (建 8 张玩家表 + player_state 落库表)。
#
# 扩容语义: 起步 8 库共享单实例; 扩容=起新实例+搬库+改 SS URL, 算法零改动。
# 详见 docs/architecture/scale-to-fit.md。
# ============================================================================
set -euo pipefail

MYSQL_CMD="mysql -uroot -p${MYSQL_ROOT_PASSWORD:-cami_dev_2026}"

echo "[init-mini] creating 8 logical shard databases on single instance..."
for i in 0 1 2 3 4 5 6 7; do
  $MYSQL_CMD -e "CREATE DATABASE IF NOT EXISTS cami_shard_${i} CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
  echo "[init-mini] cami_shard_${i} created, applying schema..."
  # docker-entrypoint-initdb.d 下的 init-shard.sql 由本脚本显式执行 (同名挂载为只读)
  $MYSQL_CMD cami_shard_${i} < /docker-entrypoint-initdb.d/init-shard.sql
done

echo "[init-mini] creating global database..."
$MYSQL_CMD -e "CREATE DATABASE IF NOT EXISTS cami_global CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci"
if [ -f /docker-entrypoint-initdb.d/init-global.sql ]; then
  $MYSQL_CMD cami_global < /docker-entrypoint-initdb.d/init-global.sql
fi

echo "[init-mini] done: cami_shard_0..7 + cami_global ready on single instance"
