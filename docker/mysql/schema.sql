-- ============================================================================
-- CAMI 数据库 Schema (Canonical / 8-shard design)
-- ============================================================================
-- 文档状态 : [PRODUCTION] 设计稿 (Week 4 Day 1 — 2026-08-31)
-- 引擎     : MySQL 8.0 | InnoDB | utf8mb4 / utf8mb4_unicode_ci
-- 分片键   : player_id (所有玩家业务表统一) —— ADR-003
-- 规范来源 : docs/architecture/architecture-spec.md §5.2 / ADR-003
-- 3NF 校验 : docs/database/er-player-sharding.md §3NF 校验
--
-- ── 数据库划分 ────────────────────────────────────────────────────────────
--   cami_global  : 全局未分片库 (账号域 + 拍卖/公会 + 分片支撑表)
--   cami_shard_N : 玩家分片库, N = 0..7 (8 分库), 仅含玩家业务表
--
-- ── 部署拆分 ──────────────────────────────────────────────────────────────
--   本文件为"可读权威版"；实际部署拆分为两份逐库脚本:
--     docker/mysql/init-global.sql  -> cami_global
--     docker/mysql/init-shard.sql   -> cami_shard_0..7 (每个分片库执行同一份)
--   分片数量 (8) 与路由算法在 ShardingSphere 配置中声明 (见 Tuesday 任务,
--   config-cami_db.yaml 的 actualDataNodes: ds_${0..7}, algorithm: player_id % 8)。
--
-- ── 设计要点 ──────────────────────────────────────────────────────────────
--   1. 所有玩家业务表以 player_id 为分片键, 且为 PRIMARY KEY 首列
--      (ShardingSphere 单分片路由 + 避免广播的基本要求)。
--   2. 账号域 (account) 先于角色存在, 无法以 player_id 路由, 故落在全局库;
--      其支撑表 account_character / player_name_reservation 解决
--      "账号→角色列举" 与 "角色名跨分片唯一" 两个分片固有问题。
--      账号与角色号分离: 单账号最多创建 n 个角色 (当前 n=5, 后续可扩 8-10)。
--      上限由配置中心驱动, 经 Data Service 原子条件更新
--      `UPDATE account SET character_count=character_count+1
--        WHERE account_id=? AND character_count < n` 拦截
--      (affected_rows=0 即拒绝); DB 层仅加宽松 CHECK 防脏数据, 不硬编码 n。
--   3. 货币以整数最小单位存储 (如铜币), 禁用 FLOAT / DECIMAL 浮点 (金额红线)。
--   4. 每条玩家记录携带 version 乐观锁, 写入经 CAS 防多节点并发覆盖 (架构 §5.3)。
-- ============================================================================

SET NAMES utf8mb4;
SET FOREIGN_KEY_CHECKS = 0;

-- ============================================================================
-- 一、cami_global —— 全局未分片库
-- ============================================================================
-- 账号域无法按 player_id 分片 (账号先于角色), 故集中在全局库。
-- 分片支撑表用于收口"跨分片唯一性 / 账号→角色映射", 避免登录时广播 8 分库。

-- ----------------------------------------------------------------------------
-- 1.1 账号表 (账号) —— 全局, 不按 player_id 分片
-- ----------------------------------------------------------------------------
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

-- ----------------------------------------------------------------------------
-- 1.2 账号↔角色映射 (分片支撑) —— 全局
--     解决: 角色按 player_id 分片, 登录时需"某账号下全部角色",
--           若不落地映射则需广播 8 分库。此表把映射收口到全局库, O(1) 列举。
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `account_character` (
    `account_id`  BIGINT UNSIGNED NOT NULL,
    `player_id`   BIGINT UNSIGNED NOT NULL,
    `create_time` DATETIME DEFAULT CURRENT_TIMESTAMP,
    `deleted_at`  DATETIME DEFAULT NULL,
    PRIMARY KEY (`account_id`, `player_id`),
    UNIQUE KEY `idx_player` (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='账号-角色映射 (全局, 支持登录列举)';

-- ----------------------------------------------------------------------------
-- 1.3 角色名全局预留 (分片支撑) —— 全局
--     解决: 角色名需全服唯一, 但角色按 player_id 分片, 分片内 UNIQUE 无法跨
--           8 库约束。创角流程: 先在此表占名 (PK name), 成功后再落分片库。
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `player_name_reservation` (
    `name`        VARCHAR(64)  NOT NULL,
    `player_id`   BIGINT UNSIGNED NOT NULL,
    `account_id`  BIGINT UNSIGNED NOT NULL,
    `create_time` DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`name`),
    UNIQUE KEY `idx_player` (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='角色名全局唯一预留 (跨分片唯一性)';

-- ----------------------------------------------------------------------------
-- 1.4 拍卖行 / 公会 (全局, 不按 player_id 分片) —— 沿用 init-global.sql
--     拍卖行 / 公会需要全局查询, 不适合按 PlayerID 分片 (ADR-003)。
--     (auction_listing / auction_history / guild / guild_member 见 init-global.sql)
-- ----------------------------------------------------------------------------


-- ============================================================================
-- 二、cami_shard_N —— 玩家分片库 (N = 0..7, 8 分库)
--     所有表均以 player_id 为分片键, 且为 PRIMARY KEY 首列。
-- ============================================================================

-- ----------------------------------------------------------------------------
-- 2.1 角色基础表 (角色) —— 分片键 player_id
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `player_base` (
    `player_id`   BIGINT UNSIGNED NOT NULL                COMMENT 'PlayerID (分片键 / PK首列)',
    `account_id`  BIGINT UNSIGNED NOT NULL                COMMENT '所属账号 (逻辑引用 cami_global.account)',
    `name`        VARCHAR(64)     NOT NULL                COMMENT '角色名 (全服唯一, 由 player_name_reservation 保障)',
    `level`       INT UNSIGNED    DEFAULT 1,
    `exp`         BIGINT UNSIGNED DEFAULT 0,
    `class_id`    INT UNSIGNED    NOT NULL                COMMENT '职业 (引用全局 class 配置, 非派生属性)',
    `scene_id`    INT UNSIGNED    DEFAULT 0               COMMENT '当前所在场景 (场景线程归属)',
    `create_time` DATETIME        DEFAULT CURRENT_TIMESTAMP,
    `login_time`  DATETIME        DEFAULT NULL,
    `logout_time` DATETIME        DEFAULT NULL,
    `deleted_at`  DATETIME        DEFAULT NULL            COMMENT '软删除 (角色销毁)',
    `version`     INT UNSIGNED    DEFAULT 0               COMMENT '乐观锁版本号',
    PRIMARY KEY (`player_id`),
    KEY `idx_account` (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='角色基础信息 (分片键 player_id)';
-- 说明: name 不在此建 UNIQUE —— 跨分片唯一性由 cami_global.player_name_reservation 保障。

-- ----------------------------------------------------------------------------
-- 2.2 玩家序列化状态表 (角色) —— 分片键 player_id
--     Data Service 落库专用: 存储 character 模块序列化的整行玩家状态 blob。
--     与 player_base (结构化基础信息, 角色创建/登录模块写) 分离, 互不覆盖。
--     方案 B: 本表 version 为 MySQL 权威版本源, Cas 写经 UPDATE ... WHERE version=? 裁决。
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `player_state` (
    `player_id`  BIGINT UNSIGNED NOT NULL                COMMENT 'PlayerID (分片键 / PK首列)',
    `payload`    MEDIUMBLOB     NOT NULL                COMMENT '序列化玩家行 (protobuf/blob, Data Service 不解析)',
    `version`    BIGINT UNSIGNED NOT NULL DEFAULT 0     COMMENT '乐观锁版本号 (DB 权威版本源, CAS 用)',
    `updated_at` DATETIME        DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    PRIMARY KEY (`player_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='玩家序列化状态 (Data Service 落库专用)';

-- ----------------------------------------------------------------------------
-- 2.3 背包表 (背包) —— 分片键 player_id
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `player_inventory` (
    `player_id`  BIGINT UNSIGNED NOT NULL                COMMENT 'PlayerID (分片键 / PK首列)',
    `slot`       INT UNSIGNED    NOT NULL                COMMENT '背包槽位索引',
    `item_id`    INT UNSIGNED    NOT NULL                COMMENT '物品ID (引用全局物品配置)',
    `item_count` INT UNSIGNED    DEFAULT 1               COMMENT '堆叠数量',
    `item_data`  BLOB            DEFAULT NULL            COMMENT '物品附加属性 (FlatBuffers 序列化, 不透明原子列)',
    `version`    INT UNSIGNED    DEFAULT 0               COMMENT '乐观锁版本号',
    PRIMARY KEY (`player_id`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='背包物品 (分片键 player_id)';
-- 一个槽位 = 一个堆叠; 非堆叠物品 count=1。item_id 为引用而非派生属性 -> 满足 3NF。

-- ----------------------------------------------------------------------------
-- 2.3 货币表 (货币) —— 分片键 player_id
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `player_currency` (
    `player_id`     BIGINT UNSIGNED NOT NULL             COMMENT 'PlayerID (分片键 / PK首列)',
    `currency_type` INT UNSIGNED    NOT NULL             COMMENT '货币类型 (引用全局货币配置)',
    `amount`        BIGINT UNSIGNED NOT NULL DEFAULT 0   COMMENT '余额, 整数最小单位(铜), 禁用 FLOAT/DECIMAL 浮点',
    `version`       INT UNSIGNED    DEFAULT 0            COMMENT '乐观锁版本号',
    PRIMARY KEY (`player_id`, `currency_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='货币记录 (分片键 player_id)';
-- 货币以整数最小单位存储 (如铜币), 杜绝浮点误差 (架构红线 + 金额规范)。

-- ----------------------------------------------------------------------------
-- 2.4 邮件表 (邮件) —— 分片键 player_id (收件人)
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS `player_mail` (
    `mail_id`     BIGINT UNSIGNED NOT NULL AUTO_INCREMENT COMMENT '邮件ID (分片内自增, 局部主键)',
    `player_id`   BIGINT UNSIGNED NOT NULL                COMMENT '收件人 PlayerID (分片键)',
    `sender_id`   BIGINT UNSIGNED DEFAULT 0               COMMENT '发件人 PlayerID (可能跨分片)',
    `sender_name` VARCHAR(64)     DEFAULT ''              COMMENT '发件人名(发送时快照, 跨分片防 JOIN, 非实时引用)',
    `title`       VARCHAR(128)    NOT NULL,
    `content`     TEXT,
    `attachments` BLOB            DEFAULT NULL            COMMENT '附件 (FlatBuffers 编码)',
    `is_read`     TINYINT         DEFAULT 0,
    `is_claimed`  TINYINT         DEFAULT 0,
    `create_time` DATETIME        DEFAULT CURRENT_TIMESTAMP,
    `expire_time` DATETIME        DEFAULT NULL,
    `version`     INT UNSIGNED    DEFAULT 0               COMMENT '乐观锁版本号',
    PRIMARY KEY (`mail_id`),
    KEY `idx_player` (`player_id`, `is_read`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='玩家邮件 (分片键 player_id)';
-- 反范式说明: sender_name 依赖 sender_id (非键), 严格说违反 3NF;
-- 因发件人可能位于其它分片, 跨分片 JOIN 不可行, 故保存发送时刻快照。详见 er-player-sharding.md。

-- ----------------------------------------------------------------------------
-- 2.5 装备 / 任务 / 技能 / 社交 (分片键 player_id) —— 沿用 init-shard.sql
--     本周 (D1) 不细化, 已存在于 init-shard.sql, 与以上表同构 (PK 含 player_id)。
--     (player_equipment / player_quest / player_skill / player_social)
-- ----------------------------------------------------------------------------

SET FOREIGN_KEY_CHECKS = 1;
