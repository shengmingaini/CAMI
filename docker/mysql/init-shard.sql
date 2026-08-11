-- CAMI MySQL Shard Init Script
-- Applied to each player shard database (cami_shard_1, cami_shard_2, ...)
-- Sharding key: player_id (managed by ShardingSphere)

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- Player base info
CREATE TABLE IF NOT EXISTS `player_base` (
    `player_id`   BIGINT UNSIGNED NOT NULL                COMMENT 'PlayerID (shard key / PK首列)',
    `account_id`  BIGINT UNSIGNED NOT NULL                COMMENT '所属账号 (逻辑引用 cami_global.account)',
    `name`        VARCHAR(64)     NOT NULL                COMMENT '角色名 (全服唯一, 由 player_name_reservation 保障)',
    `level`       INT UNSIGNED    DEFAULT 1,
    `exp`         BIGINT UNSIGNED DEFAULT 0,
    `class_id`    INT UNSIGNED    NOT NULL                COMMENT '职业 (引用全局 class 配置)',
    `scene_id`    INT UNSIGNED    DEFAULT 0               COMMENT '当前所在场景 (场景线程归属)',
    `create_time` DATETIME        DEFAULT CURRENT_TIMESTAMP,
    `login_time`  DATETIME        DEFAULT NULL,
    `logout_time` DATETIME        DEFAULT NULL,
    `deleted_at`  DATETIME        DEFAULT NULL            COMMENT '软删除 (角色销毁)',
    `version`     INT UNSIGNED    DEFAULT 0               COMMENT 'Optimistic lock version',
    PRIMARY KEY (`player_id`),
    KEY `idx_account` (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player base info (shard key player_id)';
-- 注意: name 不在此建 UNIQUE —— 跨分片唯一性由 cami_global.player_name_reservation 保障。

-- Player inventory
CREATE TABLE IF NOT EXISTS `player_inventory` (
    `player_id`  BIGINT UNSIGNED NOT NULL                COMMENT 'PlayerID (shard key / PK首列)',
    `slot`       INT UNSIGNED    NOT NULL                COMMENT '背包槽位索引',
    `item_id`    INT UNSIGNED    NOT NULL                COMMENT '物品ID (引用全局物品配置)',
    `item_count` INT UNSIGNED    DEFAULT 1               COMMENT '堆叠数量',
    `item_data`  BLOB            DEFAULT NULL            COMMENT 'Item extra attributes (FlatBuffers)',
    `version`    INT UNSIGNED    DEFAULT 0,
    PRIMARY KEY (`player_id`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player inventory (shard key player_id)';

-- Player currency
CREATE TABLE IF NOT EXISTS `player_currency` (
    `player_id`     BIGINT UNSIGNED NOT NULL             COMMENT 'PlayerID (shard key / PK首列)',
    `currency_type` INT UNSIGNED    NOT NULL             COMMENT '货币类型 (引用全局货币配置)',
    `amount`        BIGINT UNSIGNED NOT NULL DEFAULT 0   COMMENT '余额, 整数最小单位(铜), 禁用 FLOAT/DECIMAL 浮点',
    `version`       INT UNSIGNED    DEFAULT 0,
    PRIMARY KEY (`player_id`, `currency_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player currency (shard key player_id)';

-- Player equipment
CREATE TABLE IF NOT EXISTS `player_equipment` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `slot`         INT UNSIGNED NOT NULL,
    `item_id`      INT UNSIGNED NOT NULL,
    `item_data`    BLOB DEFAULT NULL,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`player_id`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player equipment slots';

-- Player quest
CREATE TABLE IF NOT EXISTS `player_quest` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `quest_id`     INT UNSIGNED NOT NULL,
    `status`       TINYINT UNSIGNED DEFAULT 0 COMMENT '0=not started, 1=in progress, 2=completed, 3=abandoned',
    `progress`     INT UNSIGNED DEFAULT 0,
    `accept_time`  DATETIME DEFAULT NULL,
    `complete_time` DATETIME DEFAULT NULL,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`player_id`, `quest_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player quest progress';

-- Player skill
CREATE TABLE IF NOT EXISTS `player_skill` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `skill_id`     INT UNSIGNED NOT NULL,
    `level`        INT UNSIGNED DEFAULT 1,
    `cooldown_end` DATETIME DEFAULT NULL,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`player_id`, `skill_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player skill data';

-- Player mail
CREATE TABLE IF NOT EXISTS `player_mail` (
    `mail_id`     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '邮件ID (分片内自增, 局部主键)',
    `player_id`   BIGINT UNSIGNED NOT NULL                COMMENT '收件人 PlayerID (shard key)',
    `sender_id`   BIGINT UNSIGNED DEFAULT 0               COMMENT '发件人 PlayerID (可能跨分片)',
    `sender_name` VARCHAR(64)     DEFAULT ''              COMMENT '发件人名(发送时快照, 跨分片防 JOIN, 非实时引用)',
    `title`       VARCHAR(128)    NOT NULL,
    `content`     TEXT,
    `attachments` BLOB            DEFAULT NULL            COMMENT 'FlatBuffers encoded attachments',
    `is_read`     TINYINT DEFAULT 0,
    `is_claimed`  TINYINT DEFAULT 0,
    `create_time` DATETIME DEFAULT CURRENT_TIMESTAMP,
    `expire_time` DATETIME DEFAULT NULL,
    `version`     INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`mail_id`),
    KEY `idx_player` (`player_id`, `is_read`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player mail (shard key player_id)';

-- Player social
CREATE TABLE IF NOT EXISTS `player_social` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `target_id`    BIGINT UNSIGNED NOT NULL,
    `relation_type` TINYINT UNSIGNED NOT NULL COMMENT '1=friend, 2=block',
    `create_time`  DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`player_id`, `target_id`, `relation_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player social relations';

SET FOREIGN_KEY_CHECKS = 1;
