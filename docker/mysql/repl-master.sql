-- CAMI MySQL 主库复制初始化 (挂载到每个分片主库)
-- 在每个 shard master 首次启动时创建复制账号 (GTID 复制使用)
-- 注意: 此脚本在 master 的 init-shard.sql (建表 DDL) 之后执行,
--       建表 DDL 已写入 binlog, 从库经 GTID AUTO_POSITION 自动重放。

SET NAMES utf8mb4;
CREATE USER IF NOT EXISTS 'repl'@'%' IDENTIFIED BY 'cami_dev_2026';
GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%';
FLUSH PRIVILEGES;
