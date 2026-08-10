-- CAMI MySQL Shard Init Script
-- Applied to each player shard database (cami_shard_1, cami_shard_2, ...)
-- Sharding key: player_id (managed by ShardingSphere)

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- Player base info
CREATE TABLE IF NOT EXISTS `player_base` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'PlayerID (shard key)',
    `account_id`   BIGINT UNSIGNED NOT NULL,
    `name`         VARCHAR(64) NOT NULL,
    `level`        INT UNSIGNED DEFAULT 1,
    `exp`          BIGINT UNSIGNED DEFAULT 0,
    `class_id`     INT UNSIGNED NOT NULL,
    `create_time`  DATETIME DEFAULT CURRENT_TIMESTAMP,
    `login_time`   DATETIME DEFAULT NULL,
    `logout_time`  DATETIME DEFAULT NULL,
    `version`      INT UNSIGNED DEFAULT 0 COMMENT 'Optimistic lock version',
    PRIMARY KEY (`player_id`),
    UNIQUE KEY `idx_name` (`name`),
    KEY `idx_account` (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player base info';

-- Player inventory
CREATE TABLE IF NOT EXISTS `player_inventory` (
    `id`           BIGINT UNSIGNED AUTO_INCREMENT,
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `slot`         INT UNSIGNED NOT NULL,
    `item_id`      INT UNSIGNED NOT NULL,
    `item_count`   INT UNSIGNED DEFAULT 1,
    `item_data`    BLOB DEFAULT NULL COMMENT 'Item extra attributes (FlatBuffers)',
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`id`),
    UNIQUE KEY `idx_player_slot` (`player_id`, `slot`),
    KEY `idx_player` (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player inventory';

-- Player currency
CREATE TABLE IF NOT EXISTS `player_currency` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `currency_type` INT UNSIGNED NOT NULL,
    `amount`       BIGINT UNSIGNED DEFAULT 0,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`player_id`, `currency_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player currency';

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
    `id`           BIGINT UNSIGNED AUTO_INCREMENT,
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `sender_id`    BIGINT UNSIGNED DEFAULT 0,
    `sender_name`  VARCHAR(64) DEFAULT '',
    `title`        VARCHAR(128) NOT NULL,
    `content`      TEXT,
    `attachments`  BLOB DEFAULT NULL COMMENT 'FlatBuffers encoded attachments',
    `is_read`      TINYINT DEFAULT 0,
    `is_claimed`   TINYINT DEFAULT 0,
    `create_time`  DATETIME DEFAULT CURRENT_TIMESTAMP,
    `expire_time`  DATETIME DEFAULT NULL,
    `version`      INT UNSIGNED DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_player` (`player_id`, `is_read`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player mail';

-- Player social
CREATE TABLE IF NOT EXISTS `player_social` (
    `player_id`    BIGINT UNSIGNED NOT NULL COMMENT 'Shard key',
    `target_id`    BIGINT UNSIGNED NOT NULL,
    `relation_type` TINYINT UNSIGNED NOT NULL COMMENT '1=friend, 2=block',
    `create_time`  DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`player_id`, `target_id`, `relation_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player social relations';

SET FOREIGN_KEY_CHECKS = 1;
