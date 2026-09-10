# TASK-028 · MySQL Schema 说明（SCHEMA.md）

> 权威来源：`database/migrations/*.sql`（**禁止手工改表**，§4）。
> 表结构变更只能新增一个 `NNN_*.sql` 迁移并用 `bin/mysql_migrate up` 执行。

## 1. 分片模型

| 概念 | 取值 | 说明 |
|---|---|---|
| 分片单位 | **逻辑库**（一个分片 = 一个 database） | `mmo_shard0` … `mmo_shard7` |
| 初始容量 | 8 | `ShardConfig.shard_count` 初始值；**非**硬限制，业务层不可见（§4 / §21） |
| 路由 | `business_id % shard_count` | 可插拔（`ShardConfig::shard_func`）；越界结果被取模收敛 |
| 端点 | `MySqlConfig::endpoints` | 每分片一个 `host/port/database/user`；可同实例也可分散多实例 |
| 事务边界 | **单分片** | `BatchSave` 按分片分组；`BatchSaveAtomic` 跨分片直接 `INVALID_ARGUMENT`（§9 / §20.5） |

**每张表都在每个分片库里各存一份**，结构完全相同。

## 2. 逻辑表

### 通用列（所有表都有，§8）

| 列 | 类型 | 作用 |
|---|---|---|
| `version` | `INT UNSIGNED NOT NULL DEFAULT 0` | **乐观锁**（§20.4）。写入判定见下表 |
| `updated_at` | `TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP` | 审计/调试；服务端时钟，不接受客户端时间 |

写入策略（由 `VersionCheck` 决定，`sql_builder.h` 的 `DecideSaveMode`）：

| `VersionCheck` | SQL | 语义 |
|---|---|---|
| `{0, true}`（`Save`/`Delete` 默认） | `INSERT` / `DELETE ... AND version=0` | 期望「不存在」；已存在 → 1062 → `VERSION_CONFLICT` |
| `{n>0, true}` | `UPDATE ... WHERE key=? AND version=n` | 期望实际版本为 `n`；`affected_rows==0` → `VERSION_CONFLICT` |
| `{*, false}` | `INSERT ... ON DUPLICATE KEY UPDATE` | 不校验，直接覆盖 |

> 连接开启 `CLIENT_FOUND_ROWS`，故 `affected_rows` 语义是**匹配行数**而非「实际变更行数」，
> 避免「匹配到但值未变」被误判为版本冲突。

### 2.1 `account` — 账号

| 列 | 类型 | 说明 |
|---|---|---|
| `account_id` | `BIGINT UNSIGNED` PK | 分片键 |
| `username` | `VARCHAR(64)` UNIQUE | 登录名 |
| `password_hash` | `VARCHAR(255)` | **只存 argon2id salted hash**（§15.9 / §20.7）；明文/弱哈希在编解码层被拒 |
| `created_at` | `TIMESTAMP` | 注册时间 |

### 2.2 `` `character` `` — 角色（表名是 MySQL 保留字，必须反引号）

| 列 | 类型 | 说明 |
|---|---|---|
| `char_id` | `BIGINT UNSIGNED` PK | 分片键 |
| `account_id` | `BIGINT UNSIGNED` | 归属账号（`idx_character_account`） |
| `name` | `VARCHAR(64)` | 角色名（`idx_character_name`） |
| `level` | `INT UNSIGNED` | 等级（默认 1） |
| `exp` | `BIGINT UNSIGNED` | 经验 |
| `attrs_json` | `TEXT NULL` | 属性快照（**业务层负责编解码**，接口不含业务语义，§21） |

### 2.3 `inventory` — 背包（多行表）

| 列 | 类型 | 说明 |
|---|---|---|
| `char_id` | `BIGINT UNSIGNED` PK(1/2) | **复合主键首列 = 分片键**（保证同角色子行必同分片） |
| `slot` | `INT UNSIGNED` PK(2/2) | 格子号 |
| `item_guid` | `BIGINT UNSIGNED` | 实例唯一 ID |
| `item_def_id` | `INT UNSIGNED` | 物品定义 ID |
| `` `count` `` | `INT UNSIGNED` | 堆叠数量（保留字，反引号） |
| `durability` | `INT UNSIGNED` | 耐久 |

### 2.4 `equipment` — 装备（多行表）

`char_id` + `slot` 复合主键；`item_guid` 指向背包实例。

### 2.5 `quest` — 任务进度（多行表）

| 列 | 类型 | 说明 |
|---|---|---|
| `char_id` | `BIGINT UNSIGNED` PK(1/2) | 分片键 |
| `quest_id` | `INT UNSIGNED` PK(2/2) | 任务定义 ID |
| `status` | `INT UNSIGNED` | 0=进行中 1=可交 2=已完成 |
| `progress_json` | `TEXT NULL` | 进度明细（业务编解码） |

### 2.6 `guild` — 公会

`guild_id` PK；`name` UNIQUE；`leader_id` / `member_count`。

### 2.7 `mail` — 邮件

| 列 | 类型 | 说明 |
|---|---|---|
| `mail_id` | `BIGINT UNSIGNED` PK | |
| `receiver_id` | `BIGINT UNSIGNED` | `idx_mail_receiver (receiver_id, status)` 主要查询路径 |
| `sender_id` | `BIGINT UNSIGNED` | 0 = 系统邮件 |
| `payload` | `TEXT NULL` | 附件/正文（业务编解码） |
| `status` | `INT UNSIGNED` | 0=未读 1=已读 2=已领取 |
| `expire_at` | `TIMESTAMP NULL` | **NULL = 永不过期**。空串绑定到 TIMESTAMP 会触发 1292，编解码层把空值绑定为 SQL `NULL` |

### 2.8 `kv_store` — 通用键值表

`IDataStore` 的业务无关落地（TASK-026 接口不含业务语义）：

| 列 | 类型 |
|---|---|
| `k` | `VARCHAR(191)` PK（`<domain>:<id>`，191 × utf8mb4 ≈ 764 字节，在索引长度上限内） |
| `payload` | `LONGBLOB NOT NULL`（**二进制安全**，含内嵌 NUL；不走字符集转换） |
| `version` / `updated_at` | 同通用列 |

### 2.9 `schema_migrations` — 迁移记录（由迁移工具维护）

| 列 | 类型 | 说明 |
|---|---|---|
| `version` | `INT UNSIGNED` PK | 从文件名前缀解析（`001_xx.sql` → 1） |
| `name` | `VARCHAR` | 文件名 |
| `success` | `TINYINT` | 1 = 成功；**0 = 失败补偿记录**（半应用状态可见，禁止静默） |
| `error` | `TEXT NULL` | 失败原因 |
| `applied_at` | `TIMESTAMP` | 执行时刻 |

## 3. 迁移

```
bin/mysql_migrate status    --shards=8 --db-prefix=mmo_shard   # 各分片状态（只读）
bin/mysql_migrate dry-run   --shards=8 --db-prefix=mmo_shard   # 将执行的清单（只读，不建库）
bin/mysql_migrate up        --shards=8 --db-prefix=mmo_shard   # 执行（按版本升序，幂等）
```

语义要点：

- **只读保证**：`status` / `dry-run` 不建库、不写表；库不存在时如实报告「全部 pending」。
- **幂等**：已完成版本跳过；重跑 `up` 返回 `applied 0`。
- **失败即中止 + 补偿记录**：MySQL 的 DDL 隐式提交、无法回滚，故失败版本写 `success=0` + `error`
  后立即中止，任一分片失败都不算成功（禁止「部分分片已升级」被当作成功）。
- **迁移文件命名**：`NNN_name.sql`，前缀必须是数字且 ≥ 1；重复版本号在 `Discover` 阶段即报错。
- **禁止手工改表**（§4）。

### 迁移文件清单

| 版本 | 文件 | 内容 |
|---|---|---|
| 001 | `001_init.sql` | `kv_store` + 7 张逻辑表 + `schema_migrations`（共 8 条语句） |
| 002 | `002_mail_expire_index.sql` | `mail.expire_at` 过期扫描索引（幂等写法：`information_schema` 判断 + `PREPARE` 动态 DDL；共 5 条语句） |

## 4. 分片键选择

| 表 | 分片键 | 理由 |
|---|---|---|
| `kv_store` | `k` 中的 `<id>` 段 | 与业务 ID 对齐，保证同一实体的数据集中 |
| `account` / `character` | `account_id` / `char_id` | 按玩家路由 |
| `inventory` / `equipment` / `quest` | `char_id`（复合主键首列） | 同角色子树行必落同一分片 → 单分片事务可覆盖 |
| `guild` | `guild_id` | 公会为独立聚合 |
| `mail` | `mail_id`；查询走 `(receiver_id, status)` | 邮件量大，索引优先于共置 |

## 5. 本地实例

```
docker compose -f docker/mysql/docker-compose.yml up -d
# 或 Windows 原生：archive.mariadb.org/mariadb-11.8.6/winx64-packages/mariadb-11.8.6-winx64.zip
```

口令经环境变量注入（`MySqlConfig::password_env`，默认 `MMORPG_MYSQL_PASSWORD`）；
**名字给了但变量未设置即明确失败**，禁止空密码兜底。本地无密码实例用
`--password-env=`（空）显式声明。
