---
TASK-ID: TASK-028
NAME: MySQL Adapter
PHASE: Phase 6 · 数据系统
MODULE: server/dataservice
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-026
---

# TASK-028 · MySQL Adapter

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-028` |
| NAME | MySQL Adapter |
| PHASE | Phase 6 · 数据系统 |
| MODULE | `server/dataservice` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-026` |

---

## 1. Objective

实现 MySQL 适配器：MySQLClient / ConnectionPool / Repository / Transaction / Migration / Schema。初始逻辑表 Account / Character / Inventory / Equipment / Quest / Guild / Mail。**初始 8 个逻辑 shard 只是容量测试方案，禁止写死到业务层。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-026` · DataService Interface

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 026`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/dataservice`

## 4. State Owner（状态归属）

MySQL 是最终持久化权威。分片路由（shard 选择）由 DataService 内部的分片策略独占决定，业务层禁止感知分片数、禁止写死 8。Schema 迁移只能由本任务提供的迁移工具执行，禁止手工改表。密码只存 salted hash，禁止明文或可逆加密。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-026 IDataStore / IRepository 接口；本地或容器 MySQL 实例

## 6. Output

MySQL 适配器 + 建表迁移脚本 + 分片路由 + 集成测试（需真实 MySQL）

## 7. Public Interface

```cpp
namespace mmo::data::mysql {
struct ShardConfig { uint32_t shard_count{8};                 // 初始容量，非硬限制
                     std::function<uint32_t(uint64_t)> shard_func; };  // 默认 id % shard_count
struct MySqlConfig { std::vector<ShardEndpoint> endpoints;    // 每分片一个 endpoint
                     size_t pool_size_per_shard{4};
                     DurationMs connect_timeout{3000}; DurationMs query_timeout{1000};
                     std::string password_env{"MMORPG_MYSQL_PASSWORD"}; };
class ShardRouter { public:                                   // 分片路由，业务层只传业务 ID
  uint32_t ShardOf(uint64_t business_id) const noexcept;
  const ShardEndpoint& EndpointOf(uint32_t shard) const;
  core::Result<void> Reshard(uint32_t new_count, const ReshardPlan&);   // 预留接口，第一版不实现迁移执行
  size_t ShardCount() const noexcept; };
class MySqlStore final : public IDataStore { public:          // 实现 TASK-026
  core::Result<std::optional<Record>> Load(const DataKey&) override;
  core::Result<void> Save(const Record&, VersionCheck = {}) override;
  core::Result<void> BatchSave(std::span<const Record>) override;   // 单分片内事务
  core::Result<void> Migrate(std::string_view migrations_dir);
  HealthStatus Health(uint32_t shard) const noexcept; };
template <typename T> class MySqlRepository final : public IRepository<T> { /* 通用 CRUD */ };
}
```

## 8. Data Model

**初始逻辑表（7 张）**

| 表 | 主键 | 关键字段 | 分片键 |
|---|---|---|---|
| account | account_id | username, password_hash, created_at | account_id |
| character | char_id | account_id, name, level, exp, attrs_json, version | char_id |
| inventory | char_id, slot | item_guid, item_def_id, count, durability | char_id |
| equipment | char_id | slot, item_guid, version | char_id |
| quest | char_id, quest_id | status, progress_json, version | char_id |
| guild | guild_id | name, leader_id, member_count | guild_id |
| mail | mail_id | receiver_id, sender_id, payload, status, expire_at | receiver_id |

**通用列**：每张表必须含 `version INT NOT NULL DEFAULT 0`（乐观锁）与 `updated_at TIMESTAMP`。
**迁移**：`database/migrations/NNN_xxx.sql`，版本号递增，迁移工具记录已执行的版本到 `schema_migrations` 表。

## 9. Thread Model

MySQL 访问走每分片连接池，由 DataService 的 Persistence 线程池执行。事务只在单分片内（跨分片用最终一致 + Ledger）。禁止在 GameNode Tick 内访问 MySQL（GameNode 根本不连 MySQL）。

## 10. Hot Path

**NO**


## 11. External IO

**YES**

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES**

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES**

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/dataservice/include/mmo/data/mysql/
- server/dataservice/src/mysql/
- server/dataservice/tests/
- database/
- docker/

## 15. Implementation Steps

1. 选择 MySQL C++ 客户端（mysql-connector-c++ 或 libmysqlclient），经 vcpkg 引入
2. 实现 shard_router.h/.cpp：分片路由（默认取模），业务层只传业务 ID，**禁止**业务代码感知分片数
3. 实现 connection_pool.h/.cpp：每分片独立池、断线重连、查询超时、慢查询记录
4. 实现 mysql_store.h/.cpp：实现 IDataStore，含 version 乐观锁（UPDATE ... WHERE version = ?）
5. 实现事务：单分片内 BatchSave 走事务，失败整体回滚；跨分片不支持事务（返回明确错误）
6. 实现迁移工具 tools/migrate/：按 schema_migrations 表记录版本，支持 up / status / dry-run
7. 编写 7 张表的初始迁移 SQL（database/migrations/001_init.sql）
8. 实现 Repository 模板：AccountRepo / CharacterRepo / InventoryRepo / EquipmentRepo / QuestRepo / GuildRepo / MailRepo
9. 实现密码存储：只存 salted hash（argon2 或 bcrypt），**禁止**明文或 MD5
10. 提供 docker/mysql/docker-compose.yml（8 个逻辑库可用同一实例 8 个 database 模拟，降低本地验证成本）
11. 写集成测试（标记 `[mysql]`）：CRUD、事务回滚、乐观锁冲突、迁移升级、连接池耗尽

## 16. Unit Test

ShardRouter 分片计算与边界（id=0、极大 id）；乐观锁 SQL 生成；迁移版本号解析；Repository 字段映射；密码哈希与校验（禁明文）

## 17. Integration Test

**连真实 MySQL**：7 张表全部建表成功；1000 条角色数据 CRUD；乐观锁冲突可复现（两个写者同时改同一行，第二个返回 VERSION_CONFLICT）；事务回滚验证（中途失败后数据不变）；迁移从 001 到 002 平滑升级；kill MySQL 后返回明确错误且可重连

## 18. Benchmark

bin/mysql_bench：`insert_ns=` / `select_ns=` / `batch_insert_ns_per_1k=` / `pool_acquire_ns=` / `txn_ns=`

## 19. Failure Test

MySQL 未启动：Create 返回明确错误，DataService 可启动但数据操作失败（不崩溃）；连接池耗尽：返回 BUSY 并计数，不死锁；慢查询：超时返回 TIMEOUT，记录慢日志；死锁（并发事务）：捕获 1213 错误码并重试（有限次）或返回明确错误；迁移失败：中止并记录，禁止半应用状态（用事务包裹 DDL 或记录补偿）

## 20. Acceptance Criteria

1. **连真实 MySQL 的集成测试全部通过**
2. 7 张逻辑表（Account/Character/Inventory/Equipment/Quest/Guild/Mail）建表脚本齐全
3. **分片数未被写死到业务层**（grep：业务代码无 shard_count / % 8）
4. 乐观锁（version 列）生效，冲突返回 VERSION_CONFLICT
5. 事务与回滚正确，跨分片事务被明确拒绝
6. 迁移工具可用（up / status / dry-run）
7. 密码只存 salted hash（grep 无明文密码存储）
8. Debug / Release 双构建通过，无 MySQL 时 `[mysql]` 用例明确 skip 而非伪装通过

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 GameNode 直接连接 MySQL（必须走 DataService）
- 禁止把分片数写死到业务层
- 禁止明文或弱哈希存储密码（必须 salted hash）
- 禁止跨分片事务（第一版不支持，必须明确报错）
- 禁止在 Tick 内访问 MySQL
- 禁止无 version 乐观锁的并发写
- 禁止迁移留下半应用状态

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单条主键查询 < 1ms；批量插入 1000 条 < 200ms；单分片写 QPS > 2000；连接池获取 < 5us；8 分片聚合读 QPS > 10000。

## 23. Deliverables

- server/dataservice/include/mmo/data/mysql/mysql_store.h
- server/dataservice/include/mmo/data/mysql/shard_router.h
- server/dataservice/include/mmo/data/mysql/connection_pool.h
- server/dataservice/src/mysql/*.cpp
- server/dataservice/tests/*
- database/migrations/001_init.sql
- tools/migrate/*
- docker/mysql/docker-compose.yml
- server/dataservice/docs/SCHEMA.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-028.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-028.sh
bash scripts/verify/task-028.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 026`
2. 交付物存在性检查（6 项）
3. 端口占用检查：3306
4. CMake configure + 编译（Debug + Release 双构建）
5. ctest 过滤执行：`-R DataService_MySql`
6. Benchmark 执行：`bin/mysql_bench --ops 10000`
7. 性能阈值断言：`bench/mysql.txt` 中 `select_ns` ≤ `1000000`
8. 性能阈值断言：`bench/mysql.txt` 中 `pool_acquire_ns` ≤ `5000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-028

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): MySQL Adapter

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-028.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-028
EOF

# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口
git push git@github.com:22:shengmingaini/CAMI.git main
```

提交规范：

- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `feat`）
- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交
- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述
- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过

## 26. Codex Execution Rules

1. 读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。
2. 读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。
3. 查依赖：确认 TASK-026 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-028.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-026` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

### 27.3 模块边界红线（全任务统一）

- 模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`include/` + `src/` + `tests/` + `docs/`），禁止扩散到其它任务拥有的目录。
- 下游只能通过本任务 `include/` 下的**公开头与接口**调用，禁止 `#include` 本任务 `src/` 或内部头（验收脚本会静态扫描本任务 `include/` 是否泄露内部 `src/`）。
- 本任务只调用依赖模块**声明**的接口，禁止访问其内部数据（如 `otherModule.internalData` 模式）。
- 接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。
- 依赖方向单向（Game → Gameplay → Core），禁止循环依赖；新增模块不得破坏既有依赖环约束。

### 27.4 扩展性约束（可扩展框架兼容性）

- 新增同类能力（新 Command / 新 Event / 新 Scene 类型 / 新模块）必须走**注册表 / ID 段**机制，禁止在 `switch` 里硬编码穷举。
- 跨模块扩展点统一用抽象（C++ Interface / Command / Event），新增实现**不得修改既有任务文件**。
- 协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。
- 所有模块遵循统一目录模板（include/src/tests/benchmark/docs/CMakeLists.txt）与五文档契约（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST），新增模块不得例外。

> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；
> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。

## 28. 变更记录

| 日期 | 变更 |
|---|---|
| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |
| 2026-08-29 | 完善：新增 §27 接口契约/模块边界/扩展性（全任务统一，防相互干扰）；新增 TASK-039 Social / TASK-040 ControlService / TASK-041 集成与回归；依赖相位自检跳过最终交付汇点；Scene Migration 登记为 Phase 2 RFC |
