---
TASK-ID: TASK-027
NAME: Redis Adapter
PHASE: Phase 6 · 数据系统
MODULE: server/dataservice
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-026
---

# TASK-027 · Redis Adapter

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-027` |
| NAME | Redis Adapter |
| PHASE | Phase 6 · 数据系统 |
| MODULE | `server/dataservice` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-026` |

---

## 1. Objective

实现 Redis 适配器：RedisClient / RedisRepository / ConnectionPool / Serialization / Retry / Timeout。第一版支持 Session、Cache、Routing 三类用途。

## 2. Dependencies

### 2.1 前置任务

- `TASK-026` · DataService Interface

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 026`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/dataservice`

## 4. State Owner（状态归属）

Redis 只保存非权威、可重建的数据（Session 缓存、路由缓存、排行榜、临时数据），写入权归 DataService。任何键都必须可过期或可从 MySQL 重建，禁止把 Redis 当作最终权威，禁止把 RDB Snapshot 当作唯一故障恢复方案。禁止 KEYS 命令。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-026 ICache / IDataStore 接口；本地或容器内的 Redis 实例（验收需可启动）

## 6. Output

Redis 适配器 + 连接池 + 集成测试（需真实 Redis）+ 故障降级测试

## 7. Public Interface

```cpp
namespace mmo::data::redis {
struct RedisConfig { std::string host{"127.0.0.1"}; uint16_t port{6379};
                     std::string password_env{"MMORPG_REDIS_PASSWORD"};  // 从环境变量读，禁止硬编码
                     size_t pool_size{8}; DurationMs connect_timeout{1000};
                     DurationMs op_timeout{200}; uint32_t max_retries{2};
                     uint16_t database{0}; };
class ConnectionPool { public:
  static core::Result<std::unique_ptr<ConnectionPool>> Create(RedisConfig);
  core::Result<PooledConnection> Acquire(DurationMs timeout);
  PoolStats Stats() const noexcept;   // in_use / idle / wait_count / acquire_timeout_count };
class RedisCache final : public ICache { public:    // 实现 TASK-026 的 ICache
  core::Result<std::optional<Record>> Get(const DataKey&) override;
  core::Result<void> Put(const Record&, DurationMs ttl = {}) override;
  core::Result<void> Invalidate(const DataKey&) override;
  core::Result<void> InvalidatePrefix(std::string_view) override;   // 用 SCAN，禁止 KEYS
  HealthStatus Health() const noexcept; };
class SessionStore final : public gateway::ISessionStore { public:  // 实现 TASK-009 的接口
  core::Result<void> Put(const gateway::Session&) override; /* ... */ };
}
```

## 8. Data Model

**键空间规范（统一前缀，禁止各模块自造）**

| 用途 | 键模式 | TTL | 说明 |
|---|---|---|---|
| Session | `sess:{session_id}` | 30min | 网关会话，重连用 |
| Player 路由 | `route:player:{player_id}` | 1h | PlayerID → GameNode |
| Scene 路由 | `route:scene:{scene_id}` | 1h | SceneID → GameNode |
| 角色缓存 | `cache:char:{char_id}` | 10min | cache-aside |
| 背包缓存 | `cache:inv:{char_id}` | 10min | cache-aside |

**序列化**：值统一用 Protobuf（TASK-005）或 JSON（可读性强的小对象），并在值内嵌 `version` 字段。
**禁止**：`KEYS` 命令、`FLUSHALL`、无 TTL 的缓存键（除路由类有明确清理路径的）。

## 9. Thread Model

Redis 访问走连接池，由 DataService 的 Worker 线程执行。GameNode 侧通过 RPC 异步调用，**禁止在 Tick 内同步访问 Redis**。

## 10. Hot Path

**NO**


## 11. External IO

**YES**

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES**

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES** （缓存）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/dataservice/include/mmo/data/redis/
- server/dataservice/src/redis/
- server/dataservice/tests/
- docker/
- docs/

## 15. Implementation Steps

1. 选择 C++ Redis 客户端（推荐 redis-plus-plus 或自研 RESP 极简客户端），通过 vcpkg 引入并锁定版本
2. 实现 connection_pool.h/.cpp：固定大小池、获取超时、空闲检测、断线重连、指标（wait_count / timeout_count）
3. 实现 redis_cache.h/.cpp：实现 ICache 四个方法，含 TTL 设置与前缀失效（SCAN + 批量 DEL，分批避免阻塞）
4. 实现序列化：Record ↔ Redis string，含 version 字段与校验（损坏数据返回错误而非崩溃）
5. 实现重试：网络类错误（超时/连接断开）重试最多 2 次，指数退避；业务类错误（如类型错误）不重试
6. 实现 SessionStore：实现 TASK-009 的 ISessionStore，会话 TTL 与 grace 期一致
7. 实现路由键读写：route:player / route:scene（供 TASK-010 Gateway 使用，本任务只提供存储能力）
8. 实现健康检查与熔断：连续失败 N 次进入熔断，熔断期间快速失败并返回 BUSY（防雪崩）
9. 提供 docker/redis/docker-compose.yml 与本地启动脚本，验收时**必须连真实 Redis**
10. 写集成测试（标记 `[redis]`，需真实实例）：CRUD、TTL 过期、前缀失效、连接池耗尽、熔断、重连

## 16. Unit Test

配置解析（密码从环境变量读，缺失时报错而非用空密码）；键名生成规范；Record 序列化往返与损坏数据容错；重试判定逻辑（哪些错误可重试）；熔断状态机

## 17. Integration Test

**连真实 Redis**：1000 次 Get/Put/Invalidate 混合操作全部成功；TTL 过期生效；前缀失效用 SCAN 分批（断言未使用 KEYS 命令，用 MONITOR 或慢日志验证）；连接池耗尽时返回超时而非死锁；kill Redis 后进入熔断，恢复后自动恢复

## 18. Benchmark

bin/redis_bench：`get_ns=` / `put_ns=` / `pipeline_ns_per_100=` / `pool_acquire_ns=` / `conn_count=`

## 19. Failure Test

Redis 未启动：Create 返回明确错误，服务可降级启动（缓存直连 Store）；Redis 中途宕机：操作返回错误，熔断生效，恢复后自动重连；连接池耗尽：acquire 超时返回 BUSY 并计数（不死锁）；慢查询（注入 1s 延迟）：超时返回 TIMEOUT 而非挂死；密码错误：启动即失败并有明确日志（禁止重试风暴）

## 20. Acceptance Criteria

1. **连接真实 Redis 的集成测试全部通过**（本地 docker 或 Windows 版 Redis）
2. ICache 四个方法全部实现，行为与内存实现一致（同一套接口测试）
3. Session / Cache / Routing 三类用途均可用
4. **禁止使用 KEYS 命令**（MONITOR 验证 + grep 代码）
5. 密码从环境变量读取，代码中无明文口令（grep 验证）
6. 连接池耗尽/Redis 宕机/慢查询三种故障行为明确且有测试
7. Debug / Release 双构建通过，ctest -R DataService 全绿（无 Redis 时 `[redis]` 用例应明确 skip 而非伪装通过）

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在 GameNode Tick 内同步访问 Redis
- 禁止使用 KEYS / FLUSHALL 命令
- 禁止硬编码 Redis 密码（必须环境变量）
- 禁止无 TTL 的缓存键（除有明确清理路径的路由键）
- 禁止把 Redis 当作实时游戏状态的权威 Owner
- 禁止把 Redis RDB 快照当作唯一故障恢复方案
- 禁止无熔断保护（雪崩风险）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单条 Get < 200us（本机）；Pipeline 100 条 < 1ms；连接池获取 < 1us（无争用）；1000 QPS 下 CPU 占用 < 5%。

## 23. Deliverables

- server/dataservice/include/mmo/data/redis/redis_cache.h
- server/dataservice/include/mmo/data/redis/connection_pool.h
- server/dataservice/src/redis/*.cpp
- server/dataservice/tests/*
- docker/redis/docker-compose.yml
- server/dataservice/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-027.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-027.sh
bash scripts/verify/task-027.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 026`
2. 交付物存在性检查（2 项）
3. 静态红线扫描：`server/dataservice/src/redis` 内禁止出现 /\"KEYS\"/
4. 端口占用检查：6379
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R DataService_Redis`
7. Benchmark 执行：`bin/redis_bench --ops 10000`
8. 性能阈值断言：`bench/redis.txt` 中 `get_ns` ≤ `200000`
9. 性能阈值断言：`bench/redis.txt` 中 `pool_acquire_ns` ≤ `1000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-027

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Redis Adapter

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-027.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-027
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
12. 跑验收脚本：`bash scripts/verify/task-027.sh` 退出码 0 后，才执行 §25 提交。

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
