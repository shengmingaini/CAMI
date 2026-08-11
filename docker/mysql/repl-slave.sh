#!/bin/bash
# CAMI MySQL 从库复制初始化 (挂载到每个分片从库)
# 等待本分片主库 (MASTER_HOST 环境变量) 的 repl 账号可用,
# 然后配置 GTID AUTO_POSITION 复制并启动。
#
# 从库不挂载 init-shard.sql —— 库表结构由主库 binlog 经复制自动重建,
# 避免 "从库已建表 + 重放主库 CREATE TABLE" 的主键/表已存在冲突。
#
# 注意: 从库容器启动时不带 --read_only / --super_read_only (否则 entrypoint
# 初始化阶段的 CHANGE MASTER TO / START SLAVE 会被只读拦截导致容器退出)。
# 复制建立后, 本脚本动态 SET GLOBAL read_only=ON 锁定从库, 防止应用直写。
set -e

MASTER="${MASTER_HOST:-mysql-shard-0}"
echo "[repl-slave] waiting for master ${MASTER} (repl user) ..."
until mysql -h "${MASTER}" -urepl -pcami_dev_2026 -e "SELECT 1" >/dev/null 2>&1; do
  sleep 2
done

echo "[repl-slave] master reachable, setting up replication from ${MASTER} ..."
mysql -uroot -pcami_dev_2026 <<SQL
CHANGE MASTER TO
  MASTER_HOST='${MASTER}',
  MASTER_USER='repl',
  MASTER_PASSWORD='cami_dev_2026',
  MASTER_AUTO_POSITION=1;
START SLAVE;
-- 复制启动后锁定为只读, 防止应用直写从库 (SS 经读数据源只读访问)
SET GLOBAL read_only = ON;
SQL

echo "[repl-slave] replication started for ${MASTER}."
