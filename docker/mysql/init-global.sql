-- CAMI Global DB Init Script
-- Applied to cami_global database (auction house, guild - not sharded by player_id)

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ============================================================================
-- 账号域 (全局未分片) + 分片支撑表
-- 说明: 与 docker/mysql/schema.sql (权威 DDL) 保持一致。
--       账号先于角色存在, 无法以 player_id 路由, 故落在全局库;
--       account_character / player_name_reservation 解决分片固有问题。
-- ============================================================================

-- 账号表 (账号) —— 全局, 不按 player_id 分片
CREATE TABLE IF NOT EXISTS `account` (
    `account_id`      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '账号ID (全局主键)',
    `username`        VARCHAR(64)     NOT NULL                COMMENT '登录名',
    `pass_hash`       VARBINARY(128)  NOT NULL                COMMENT '密码哈希 (Argon2id 编码串, 已含 salt+params)',
    `pass_salt`       VARBINARY(32)   DEFAULT NULL            COMMENT '独立 salt (PBKDF2/HMAC 场景; Argon2 可置 NULL)',
    `kdf_version`     TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT 'KDF 算法版本, 支持平滑轮换',
    `email`           VARCHAR(255)    DEFAULT NULL            COMMENT '绑定邮箱 (可空)',
    `status`          TINYINT UNSIGNED NOT NULL DEFAULT 1     COMMENT '1=正常 0=禁用 2=封禁',
    `character_count` INT UNSIGNED    NOT NULL DEFAULT 0      COMMENT '当前角色数 (缓存计数; 业务上限 n 由配置中心驱动, 当前 5 后续可扩 8-10; 实际拦截由 Data Service 原子更新 WHERE character_count<n 完成, 此处 CHECK 仅防脏数据)',
    `deleted_at`      DATETIME        DEFAULT NULL            COMMENT '软删除时间 (GDPR 删除权)',
    `create_time`     DATETIME        DEFAULT CURRENT_TIMESTAMP,
    `last_login_time` DATETIME        DEFAULT NULL,
    `version`         INT UNSIGNED    DEFAULT 0               COMMENT '乐观锁版本号',
    PRIMARY KEY (`account_id`),
    UNIQUE KEY `idx_username` (`username`),
    UNIQUE KEY `idx_email` (`email`),
    CONSTRAINT `chk_account_char_count` CHECK (`character_count` >= 0 AND `character_count` <= 100)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='账号 (全局未分片)';

-- 账号↔角色映射 (分片支撑): 登录时 O(1) 列举某账号下全部角色, 避免广播 8 分库
CREATE TABLE IF NOT EXISTS `account_character` (
    `account_id`  BIGINT UNSIGNED NOT NULL,
    `player_id`   BIGINT UNSIGNED NOT NULL,
    `create_time` DATETIME DEFAULT CURRENT_TIMESTAMP,
    `deleted_at`  DATETIME DEFAULT NULL,
    PRIMARY KEY (`account_id`, `player_id`),
    UNIQUE KEY `idx_player` (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='账号-角色映射 (全局, 支持登录列举)';

-- 角色名全局唯一预留 (分片支撑): 跨分片唯一性收口 (分片内 UNIQUE 无法跨 8 库)
CREATE TABLE IF NOT EXISTS `player_name_reservation` (
    `name`        VARCHAR(64)  NOT NULL,
    `player_id`   BIGINT UNSIGNED NOT NULL,
    `account_id`  BIGINT UNSIGNED NOT NULL,
    `create_time` DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`name`),
    UNIQUE KEY `idx_player` (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='角色名全局唯一预留 (跨分片唯一性)';

-- Auction house listings
CREATE TABLE IF NOT EXISTS `auction_listing` (
    `id`           BIGINT UNSIGNED AUTO_INCREMENT,
    `seller_id`    BIGINT UNSIGNED NOT NULL,
    `seller_name`  VARCHAR(64) NOT NULL,
    `item_id`      INT UNSIGNED NOT NULL,
    `item_count`   INT UNSIGNED DEFAULT 1,
    `item_data`    BLOB DEFAULT NULL COMMENT 'FlatBuffers encoded item attributes',
    `price`        BIGINT UNSIGNED NOT NULL,
    `currency_type` INT UNSIGNED DEFAULT 1,
    `status`       TINYINT UNSIGNED DEFAULT 0 COMMENT '0=active, 1=sold, 2=cancelled, 3=expired',
    `buyer_id`     BIGINT UNSIGNED DEFAULT NULL,
    `create_time`  DATETIME DEFAULT CURRENT_TIMESTAMP,
    `expire_time`  DATETIME NOT NULL,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_item` (`item_id`),
    KEY `idx_seller` (`seller_id`),
    KEY `idx_status_expire` (`status`, `expire_time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Auction house listings';

-- Auction transaction history
CREATE TABLE IF NOT EXISTS `auction_history` (
    `id`           BIGINT UNSIGNED AUTO_INCREMENT,
    `listing_id`   BIGINT UNSIGNED NOT NULL,
    `seller_id`    BIGINT UNSIGNED NOT NULL,
    `buyer_id`     BIGINT UNSIGNED NOT NULL,
    `item_id`      INT UNSIGNED NOT NULL,
    `item_count`   INT UNSIGNED DEFAULT 1,
    `price`        BIGINT UNSIGNED NOT NULL,
    `currency_type` INT UNSIGNED DEFAULT 1,
    `trade_time`   DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `idx_seller` (`seller_id`),
    KEY `idx_buyer` (`buyer_id`),
    KEY `idx_item` (`item_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Auction transaction history';

-- Guild base info
CREATE TABLE IF NOT EXISTS `guild` (
    `guild_id`     BIGINT UNSIGNED AUTO_INCREMENT,
    `name`         VARCHAR(64) NOT NULL,
    `leader_id`    BIGINT UNSIGNED NOT NULL,
    `level`        INT UNSIGNED DEFAULT 1,
    `member_count` INT UNSIGNED DEFAULT 1,
    `create_time`  DATETIME DEFAULT CURRENT_TIMESTAMP,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`guild_id`),
    UNIQUE KEY `idx_name` (`name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild base info';

-- Guild members
CREATE TABLE IF NOT EXISTS `guild_member` (
    `guild_id`     BIGINT UNSIGNED NOT NULL,
    `player_id`    BIGINT UNSIGNED NOT NULL,
    `role`         TINYINT UNSIGNED DEFAULT 0 COMMENT '0=member, 1=officer, 2=leader',
    `join_time`    DATETIME DEFAULT CURRENT_TIMESTAMP,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`guild_id`, `player_id`),
    KEY `idx_player` (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Guild members';

SET FOREIGN_KEY_CHECKS = 1;
