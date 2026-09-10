-- docker/mysql/init-shards.sql — TASK-028 首次启动时创建 8 个逻辑库（分片 = 逻辑库）
--
-- 与 ShardConfig.shard_count 初始容量一致；表结构**不在此处创建**，
-- 一律由迁移工具执行 database/migrations/*.sql（§4：禁止手工改表）。

CREATE DATABASE IF NOT EXISTS mmo_shard0 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard1 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard2 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard3 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard4 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard5 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard6 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
CREATE DATABASE IF NOT EXISTS mmo_shard7 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
