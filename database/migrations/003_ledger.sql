-- database/migrations/003_ledger.sql
--
-- TASK-030 · 经济账本 + 幂等表（§7 Public Interface / §8 Data Model / §13 Persistence）。
-- 由迁移工具执行（禁止手工改表，§4）：
--   bin/mysql_migrate --dir database/migrations up
--
-- 命名必须匹配 `NNN_name.sql`（版本号 >= 1，见 MigrationRunner::ParseVersion）；
-- 写成 `00N_ledger.sql` 会被**静默跳过**——迁移器不报错，表根本不存在，这类
-- 「构建全绿但功能未生效」的失败比报错更危险。
--
-- 约定（沿用 001_init.sql）：
--   · 迁移 SQL 全部幂等（IF NOT EXISTS）；MySQL/MariaDB 的 DDL 隐式提交，失败靠
--     schema_migrations 的补偿记录中止（§21）
--   · 表名反引号转义保留字（本文件无保留字表名，`operation` 仅作列名，仍加引号）
--   · 逻辑分片 = 逻辑库，每个分片库各自持有一份同样的逻辑表（见 docker/mysql/）
--
-- 红线（§13 / §33）：本表只由 **DataService** 写入；GameNode 禁止直连 MySQL。
-- GameNode 侧只依赖 `ledger::ILedgerStore` / `ledger::IIdemTable` 抽象。
--
-- ============================================================================
-- ⚠ 规格冲突（TASK-030 §8 表定义）—— 已按「正确性优先」处置，需人工确认
-- ============================================================================
-- §8 同时要求：`idempotency_key` 上有 **UNIQUE INDEX**，且 `timestamp_ms` 是
-- **按月分区键**。这两条在同一张表上**在 MariaDB/MySQL 中不可能同时成立**
-- （已在真实 MariaDB 11.8.6 上实测取证，见 docs/economy-failure-test-report.md §8.1）：
--   分区规则要求「每个唯一索引必须包含分区表达式的全部列」。
--   实验 A：UNIQUE(idempotency_key) + PARTITION BY RANGE(timestamp_ms) →
--           ERROR 1503 (HY000): A PRIMARY KEY must include all columns in the
--           table's partitioning function
--   实验 B：把 timestamp_ms 并入唯一键后可建表，但同一个 idempotency_key 只换一个
--           时间戳就能插两行（实测 rows_with_same_idem_key = 2）——
--           UNIQUE 兜底形同虚设，而崩溃重投恰恰会带来新时间戳。
--
-- 处置（§30 优先级：Correctness > … > Scalability）：
--   1. **保留全局 UNIQUE**，本迁移**不做按月分区**。UNIQUE 是正确性约束
--      （§21「禁止在没有 UNIQUE 索引的情况下依赖应用层去重」），分区是容量优化；
--      两者冲突时不得牺牲正确性。
--   2. `timestamp_ms` 上建普通索引（`idx_ledger_ts`），按月范围扫描/归档走索引，
--      0 CCU 阶段足够；单表容量到千万行量级再考虑分区。
--   3. 将来要分区时的**既定路径**（不改接口、不改状态模型）：
--      a) 把幂等兜底搬迁到独立的 `economy_idempotency`（本文件已建，键唯一、不分区）；
--      b) 账本表去掉 `uk_ledger_idempotency_key`，改为 `PARTITION BY RANGE
--         (timestamp_ms)`，并按分区规则把主键放宽为 `(transaction_id, timestamp_ms)`；
--      c) 账本侧的重复拦截改由 `economy_idempotency` 承担（账本本身就是 append-only
--         审计流水，重复行的判据应当是幂等表而不是账本）。
--   这三个子表结构在本文件里都已经就位，属于**预留**而不是缺失。
-- ============================================================================

-- ---------------------------------------------------------------------------
-- 1. economy_ledger —— 经济账本（append-only，永不 UPDATE / 永不 DELETE，§21）
--
-- 十六项字段与 C++ 侧 ledger::LedgerEntry 一一对应；列顺序刻意与
-- CanonicalFieldOrder() 保持一致，便于人工比对「哈希喂入顺序」与「落库顺序」。
-- prev_hash / hash 为 BINARY(32)：定长，链校验时按列直读，不做十六进制转换存储。
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS economy_ledger (
  transaction_id   BIGINT UNSIGNED NOT NULL,
  request_id       BIGINT UNSIGNED NOT NULL DEFAULT 0,
  idempotency_key  VARCHAR(191)    NOT NULL,
  player_id        BIGINT UNSIGNED NOT NULL,
  peer_id          BIGINT UNSIGNED NOT NULL DEFAULT 0,
  `operation`      TINYINT UNSIGNED NOT NULL,
  currency         INT UNSIGNED    NOT NULL DEFAULT 0,
  delta            BIGINT          NOT NULL,
  balance_after    BIGINT          NOT NULL,
  item_deltas_json TEXT            NULL,
  reason           VARCHAR(191)    NOT NULL DEFAULT '',
  `source`         VARCHAR(191)    NOT NULL DEFAULT '',
  timestamp_ms     BIGINT          NOT NULL,
  version          INT UNSIGNED    NOT NULL DEFAULT 0,
  prev_hash        BINARY(32)      NULL,
  hash             BINARY(32)      NULL,
  created_at       TIMESTAMP       NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (transaction_id),
  -- 数据库层兜底（§15.3 双保险 / §20.2）：应用层用幂等状态机拦截，这里挡住
  -- 「应用层漏了 / 进程崩在窗口期」的重复写入。冲突即发现重复，不是错误路径。
  UNIQUE KEY uk_ledger_idempotency_key (idempotency_key),
  -- 按玩家 + 时间窗对账（tools/audit/economy_audit.py / Ledger::QueryByPlayer）
  KEY idx_ledger_player_ts (player_id, timestamp_ms),
  -- 按月范围扫描 / 归档（将来若启分区，分区键即此列，见文件头 §冲突处置）
  KEY idx_ledger_ts (timestamp_ms),
  -- 链式校验按写入顺序拉取（Ledger::VerifyChain）
  KEY idx_ledger_hash (hash)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ---------------------------------------------------------------------------
-- 2. economy_idempotency —— 幂等表（IIdemTable 的 MySQL 落地，§15.2 / §15.3）
--
-- 与 C++ 侧 ledger::IdemRow 一一对应。**主键即 UNIQUE 索引**，对应：
--   · Redis 版：`SET key <row> NX` + TTL
--   · MySQL 版：本表 PRIMARY KEY + 状态列
-- 状态机与 TTL 逻辑只有一份（在 IdempotencyStore 里），此处只负责「行的存储与
-- UNIQUE 约束」，不实现任何业务语义（TASK-026 §21 的口径）。
--
-- status: 0=Fresh(不落库) 1=InFlight 2=Completed 3=Failed
--   · InFlight 必须有 deadline_ms（§21 禁止无 TTL 的幂等状态，会永久悬挂）
--   · Failed = TTL 过期的未决态，**不删除**：真实结果可能已落账，删掉就丢了
--     「不重复扣钱」的判据；由调用方查账本后决定重放（Commit）还是重做（Abort）
--   · Abort = 物理删行（回到 Fresh），不落 Failed：否则「余额不足 → 充值 →
--     同 key 重试」会被永久毒化
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS economy_idempotency (
  idempotency_key  VARCHAR(191)     NOT NULL,
  status           TINYINT UNSIGNED NOT NULL,
  deadline_ms      BIGINT           NOT NULL DEFAULT 0,
  result_json      TEXT             NULL,
  player_id        BIGINT UNSIGNED  NOT NULL DEFAULT 0,
  transaction_id   BIGINT UNSIGNED  NOT NULL DEFAULT 0,
  created_at       TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at       TIMESTAMP        NOT NULL DEFAULT CURRENT_TIMESTAMP
                                     ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (idempotency_key),
  -- 运维口径：找出长时间停在 InFlight 的悬挂行（验收项 5「无悬挂」）
  KEY idx_idem_status_deadline (status, deadline_ms),
  KEY idx_idem_player (player_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
