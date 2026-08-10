-- CAMI Global DB Init Script
-- Applied to cami_global database (auction house, guild - not sharded by player_id)

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

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
