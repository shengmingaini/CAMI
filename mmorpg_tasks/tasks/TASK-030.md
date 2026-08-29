---
TASK-ID: TASK-030
NAME: Economic Ledger / Idempotency
PHASE: Phase 6 · 数据系统
MODULE: server/gamenode/economy
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-001, TASK-005, TASK-026, TASK-028, TASK-029
---

# TASK-030 · Economic Ledger / Idempotency

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-030` |
| NAME | Economic Ledger / Idempotency |
| PHASE | Phase 6 · 数据系统 |
| MODULE | `server/gamenode/economy` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-001`, `TASK-005`, `TASK-026`, `TASK-028`, `TASK-029` |

---

## 1. Objective

实现经济账本与幂等性：TransactionID / RequestID / IdempotencyKey / Version / Ledger。**必须证明：重复请求、RPC 重试、断线、GameNode Crash、数据库重试五种场景下都不会重复扣钱、不会重复发奖励、不会复制装备。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-001` · Core Error / Result 系统
- `TASK-005` · Protocol Schema（Protobuf + FlatBuffers）
- `TASK-026` · DataService Interface
- `TASK-028` · MySQL Adapter
- `TASK-029` · Economy System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 001 005 026 028 029`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/economy`

## 4. State Owner（状态归属）

账本为 append-only 权威记录，Owner 是 DataService（MySQL）；幂等表的 InFlight 状态 Owner 是发起方 GameNode，带 TTL 自动过期。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-029 EconomyCommand（已含 transaction_id 与 idempotency_key）；TASK-026 DataService；TASK-028 MySQL

## 6. Output

ledger 模块 + 幂等表 + 五场景故障测试 + 资金守恒对账报告

## 7. Public Interface

```cpp
namespace mmo::game::economy {
struct LedgerEntry {                         // 只追加，永不修改（append-only）
  TransactionId transaction_id; core::RequestID request_id;
  std::string idempotency_key;               // 唯一索引
  PlayerId player; EconomyOp op;
  CurrencyType currency; int64_t delta; int64_t balance_after;
  std::vector<ItemDelta> item_deltas;
  std::string reason; std::string source;
  int64_t timestamp_ms; uint32_t version;
  std::array<uint8_t,32> prev_hash;          // 账本链，防篡改
  std::array<uint8_t,32> hash; };
enum class IdemStatus : uint8_t { Fresh, InFlight, Completed, Failed };
class IIdempotencyStore { public: virtual ~IIdempotencyStore() = default;
  virtual core::Result<IdemStatus> TryBegin(std::string_view key, DurationMs ttl) = 0;
  virtual core::Result<void> Commit(std::string_view key, const EconomyResult&) = 0;
  virtual core::Result<void> Abort(std::string_view key) = 0;
  virtual core::Result<std::optional<EconomyResult>> Lookup(std::string_view key) = 0; };
class Ledger { public:
  core::Result<void> Append(const LedgerEntry&);
  core::Result<void> Flush();                                  // 批量落库，异步
  core::Result<bool> VerifyChain(int64_t from_ts, int64_t to_ts);  // 哈希链校验
  core::Result< std::vector<LedgerEntry> > QueryByPlayer(PlayerId, int64_t from, int64_t to);
  LedgerStats Stats() const noexcept; };     // pending / flushed / duplicates_rejected
}
```

## 8. Data Model

**幂等键生成规则（唯一真相）**

| 场景 | IdempotencyKey 构成 |
|---|---|
| 购买 | `purchase:{player}:{shop_id}:{item_id}:{client_seq}` |
| 任务奖励 | `quest:{player}:{quest_id}` |
| 交易 | `trade:{trade_id}:{side}` |
| 邮件领取 | `mailclaim:{player}:{mail_id}` |
| RPC 重试 | **沿用首次的同一个 key**，禁止重试时重新生成 |

**幂等状态机**：`Fresh → InFlight → Completed | Failed`。`InFlight` 期间同一 key 的并发请求直接返回 BUSY（不重复执行）；`Completed` 的请求返回**首次结果**（deduplicated=true）。

**账本表**（database/migrations/00N_ledger.sql）

| 列 | 约束 |
|---|---|
| transaction_id | PRIMARY KEY |
| idempotency_key | **UNIQUE INDEX**（冲突即发现重复） |
| delta / balance_after | BIGINT，非空 |
| prev_hash / hash | BINARY(32)，链式校验 |
| timestamp_ms | BIGINT，分区键（按月） |

## 9. Thread Model

幂等判定与余额变更在 SimulationThread 同步完成（必须立即可见）；账本落盘异步投递到 Persistence 线程。落盘失败进入重试队列，**不阻塞内存态**，但必须保证最终落盘（用 in-flight 标记防丢失）。

## 10. Hot Path

**NO**


## 11. External IO

**YES** （异步落盘）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES** （跨进程数据服务）

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES**

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/economy/include/mmo/game/economy/ledger/
- server/gamenode/economy/src/ledger/
- server/gamenode/economy/tests/
- server/dataservice/src/mysql/
- database/migrations/
- docs/

## 15. Implementation Steps

1. 定义 ledger_entry.h：LedgerEntry 十六项字段（含 prev_hash/hash 链）
2. 实现 idempotency_store.h/.cpp：四状态机（Fresh/InFlight/Completed/Failed），带 TTL 防悬挂
3. 实现 Redis 版幂等存储（复用 TASK-027 连接池）+ MySQL 版（UNIQUE 索引兜底，双保险）
4. 实现 ledger.h/.cpp：Append（内存环形缓冲 + 异步批量落库）、Flush、VerifyChain、QueryByPlayer
5. 实现哈希链：每条 hash = SHA256(prev_hash + 规范化字段串)，禁止把整条记录序列化后哈希（字段顺序变化会断链）
6. 把 TASK-029 的 EconomySystem 接入幂等：Execute 开头 TryBegin，结尾 Commit，异常路径 Abort
7. 实现重试安全：RPC 重试必须携带**同一个** idempotency_key（在 RpcOptions 中透传，见 TASK-006）
8. 实现 InFlight 过期：TTL 默认 30 秒，过期后按最终状态重放（查账本决定结果）
9. 实现对账工具 tools/audit/economy_audit.py：按玩家汇总账本 delta 与当前余额比对，输出差异报告
10. 写**五场景故障测试**（见下），每个场景必须断言「余额变化次数 == 1」
11. 写资金守恒测试：1 万次随机经济操作后，Σ(所有玩家余额) + Σ(系统回收) == 初始发行量

## 16. Unit Test

幂等四状态机全路径；同一 key 并发请求返回 BUSY 且只执行一次；Completed 后重复请求返回首次结果；TTL 过期与重放；哈希链计算与校验；账本 Append 与批量 Flush；对账工具与守恒校验

## 17. Integration Test

**1 万次混合经济操作**：结束后 Σ账本 delta == Σ余额变化；哈希链完整可校验；对账工具输出零差异；幂等表无悬挂 InFlight

## 18. Benchmark

bin/ledger_bench：`idem_check_ns=` / `ledger_append_ns=` / `flush_ns_per_1k=` / `dedup_hit_ns=` / `mem_bytes_per_entry=`

## 19. Failure Test

**五场景故障测试（本任务的核心，必须全部通过）**

| 场景 | 模拟方式 | 断言 |
|---|---|---|
| 重复请求 | 同一 key 连发 10 次 | 余额只变 1 次，9 次返回 deduplicated=true |
| RPC 重试 | 注入 UNAVAILABLE 让客户端重试 3 次（同 key） | 只扣 1 次钱 |
| 断线重连 | 扣钱成功后立即断线，重连后客户端重发 | 不重复扣 |
| GameNode Crash | 内存中标记 InFlight 后强杀进程，重启 | 从 MySQL 幂等表恢复，不重复执行 |
| 数据库重试 | 落库时注入死锁/超时并重试 | 不产生两条账本记录（UNIQUE + 哈希链双校验） |

额外：装备发放重复场景 —— 同一幂等键不会产出两个 ItemGuid（**不复制装备**）。

## 20. Acceptance Criteria

1. **五个故障场景全部通过**，每个场景断言「余额/物品变化次数 == 1」
2. 幂等表有 UNIQUE 索引兜底（数据库层也挡得住）
3. 账本哈希链可校验，篡改检测生效（单测：改一条记录后 VerifyChain 返回 false）
4. 1 万次操作后资金守恒，对账工具零差异
5. InFlight 有 TTL，崩进程后能恢复，无悬挂
6. 重复请求返回首次结果而非报错（对客户端友好）
7. Debug / Release 双构建通过，ctest -R Economy_Ledger 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止 RPC 重试时重新生成 idempotency_key
- 禁止幂等状态无 TTL（会永久悬挂）
- 禁止账本记录被 UPDATE/DELETE（只能 append）
- 禁止在没有 UNIQUE 索引的情况下依赖应用层去重
- 禁止在 Tick 内同步等待账本落库
- 禁止允许负余额或重复发放奖励

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

幂等检查 < 200ns（Redis 路径 < 200us）；账本 Append < 500ns；批量 Flush 1000 条 < 50ms；单条账本内存 < 256B；重复请求拦截率 100%。

## 23. Deliverables

- server/gamenode/economy/include/mmo/game/economy/ledger/ledger.h
- server/gamenode/economy/include/mmo/game/economy/ledger/idempotency_store.h
- server/gamenode/economy/src/ledger/*.cpp
- server/gamenode/economy/tests/*
- database/migrations/00N_ledger.sql
- tools/audit/economy_audit.py
- server/gamenode/economy/docs/INTERFACE.md
- docs/economy-failure-test-report.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-030.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-030.sh
bash scripts/verify/task-030.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 001 005 026 028 029`
2. 交付物存在性检查（2 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Economy_Ledger`
5. Benchmark 执行：`bin/ledger_bench --ops 10000`
6. 性能阈值断言：`bench/ledger.txt` 中 `idem_check_ns` ≤ `200`
7. 性能阈值断言：`bench/ledger.txt` 中 `mem_bytes_per_entry` ≤ `256`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-030

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Economic Ledger / Idempotency

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-030.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-030
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
3. 查依赖：确认 TASK-001, TASK-005, TASK-026, TASK-028, TASK-029 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-030.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-001` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-005` · `protocol`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-026` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-028` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-029` · `server/gamenode/economy`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
