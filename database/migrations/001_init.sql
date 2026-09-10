-- database/migrations/001_init.sql
--
-- TASK-028 · 初始 Schema（§8 Data Model）。
-- 由迁移工具执行（禁止手工改表，§4）：
--   bin/mysql_migrate --dir database/migrations up
--
-- 约定：
--   · 每个分片库各自持有一份同样的逻辑表（分片 = 逻辑库，见 docker/mysql/docker-compose.yml）
--   · 每张表必须含 version（乐观锁）与 updated_at（§8 通用列）
--   · 复合主键的**首列恒为分片键**，故同一角色的全部子行必落同一分片（§9 单分片事务）
--   · 迁移 SQL 全部幂等（IF NOT EXISTS）；MySQL 的 DDL 隐式提交，失败靠补偿记录中止（§21）
--   · 表名 `character` 是 MySQL 保留字，必须反引号转义

-- ---------------------------------------------------------------------------
-- 通用键值表：IDataStore 的业务无关落地（TASK-026 §21：接口不含业务语义）
-- 键 = "<domain>:<id>"；payload = 业务层序列化后的二进制安全字节
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS kv_store (
  k VARCHAR(191) NOT NULL,
  payload LONGBLOB NOT NULL,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (k)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 1. account —— 账号（密码只存 argon2id salted hash，§20.7 禁止明文/弱哈希）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS account (
  account_id BIGINT UNSIGNED NOT NULL,
  username VARCHAR(64) NOT NULL,
  password_hash VARCHAR(255) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (account_id),
  UNIQUE KEY uk_account_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 2. `character` —— 角色（attrs_json 为属性快照，业务层编解码）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `character` (
  char_id BIGINT UNSIGNED NOT NULL,
  account_id BIGINT UNSIGNED NOT NULL,
  name VARCHAR(64) NOT NULL,
  level INT UNSIGNED NOT NULL DEFAULT 1,
  exp BIGINT UNSIGNED NOT NULL DEFAULT 0,
  attrs_json TEXT NULL,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (char_id),
  KEY idx_character_account (account_id),
  KEY idx_character_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 3. inventory —— 背包（复合主键首列 = 分片键 char_id）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS inventory (
  char_id BIGINT UNSIGNED NOT NULL,
  slot INT UNSIGNED NOT NULL,
  item_guid BIGINT UNSIGNED NOT NULL DEFAULT 0,
  item_def_id INT UNSIGNED NOT NULL DEFAULT 0,
  `count` INT UNSIGNED NOT NULL DEFAULT 0,
  durability INT UNSIGNED NOT NULL DEFAULT 0,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (char_id, slot)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 4. equipment —— 装备（复合主键首列 = 分片键 char_id）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS equipment (
  char_id BIGINT UNSIGNED NOT NULL,
  slot INT UNSIGNED NOT NULL,
  item_guid BIGINT UNSIGNED NOT NULL DEFAULT 0,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (char_id, slot)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 5. quest —— 任务进度（复合主键首列 = 分片键 char_id）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS quest (
  char_id BIGINT UNSIGNED NOT NULL,
  quest_id INT UNSIGNED NOT NULL,
  status INT UNSIGNED NOT NULL DEFAULT 0,
  progress_json TEXT NULL,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (char_id, quest_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 6. guild —— 公会
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS guild (
  guild_id BIGINT UNSIGNED NOT NULL,
  name VARCHAR(64) NOT NULL,
  leader_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  member_count INT UNSIGNED NOT NULL DEFAULT 0,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (guild_id),
  UNIQUE KEY uk_guild_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 7. mail —— 邮件（分片键 receiver_id）
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS mail (
  mail_id BIGINT UNSIGNED NOT NULL,
  receiver_id BIGINT UNSIGNED NOT NULL,
  sender_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  payload TEXT NULL,
  status INT UNSIGNED NOT NULL DEFAULT 0,
  expire_at TIMESTAMP NULL DEFAULT NULL,
  version INT UNSIGNED NOT NULL DEFAULT 0,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (mail_id),
  KEY idx_mail_receiver (receiver_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
