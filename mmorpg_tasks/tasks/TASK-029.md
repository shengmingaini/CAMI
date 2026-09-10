---
TASK-ID: TASK-029
NAME: Economy System
PHASE: Phase 6 · 数据系统
MODULE: server/gamenode/economy
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-017, TASK-026
---

# TASK-029 · Economy System

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-029` |
| NAME | Economy System |
| PHASE | Phase 6 · 数据系统 |
| MODULE | `server/gamenode/economy` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-017`, `TASK-026` |

---

## 1. Objective

实现经济系统：Currency / Reward / Purchase / Trade / Auction。**所有操作必须走 EconomyCommand**，例如 AddCurrency / RemoveCurrency / AddItem / RemoveItem / Transfer / Purchase。

## 2. Dependencies

### 2.1 前置任务

- `TASK-017` · Inventory / Equipment
- `TASK-026` · DataService Interface

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 017 026`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/economy`

## 4. State Owner（状态归属）

货币与物品余额的内存权威 Owner 是 Economy System（Scene 线程内），持久化权威归 DataService。所有余额变更必须走 EconomyCommand，禁止任何模块直接加减货币字段。查询走 Query，禁止产生副作用。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-017 背包（物品增减）；TASK-026 DataService（持久化）；TASK-007 CommandBus

## 6. Output

economy 模块 + 命令层 + 事务性测试（为 TASK-030 账本做准备）

## 7. Public Interface

```cpp
namespace mmo::game::economy {
using CurrencyType = uint32_t;    // 1=Gold 2=Silver 3=Gem 4=Honor（配置化）
struct EconomyCommand {                        // 所有经济操作的唯一入口
  core::RequestID request_id; core::TraceID trace_id;
  TransactionId transaction_id; std::string idempotency_key;   // **必填**
  PlayerId player; EconomyOp op; int64_t amount;
  CurrencyType currency; std::vector<ItemDelta> item_deltas;
  std::string_view reason; std::string_view source; int64_t timestamp_ms; };
enum class EconomyOp : uint8_t { AddCurrency, RemoveCurrency, AddItem, RemoveItem,
                                 Transfer, Purchase, Reward, Refund };
struct EconomyResult { bool applied{false}; bool deduplicated{false};
                       core::ErrorCode code; std::vector<ItemGuid> created_guids;
                       int64_t balance_after; uint32_t version; };
class EconomySystem { public:
  core::Result<EconomyResult> Execute(const EconomyCommand&, const scene::SceneContext&);
  core::Result<int64_t> Balance(PlayerId, CurrencyType) const noexcept;
  core::Result<void> SetPriceTable(const PriceTable&);       // 价格表配置化
  EconomyStats Stats() const noexcept;   // 操作计数、去重计数、失败计数
};
}
```

## 8. Data Model

**EconomyCommand 必带字段（缺一不可，缺则拒绝）**

| 字段 | 说明 |
|---|---|
| transaction_id | 全局唯一，写入账本 |
| idempotency_key | 幂等键，同一 key 只生效一次 |
| request_id / trace_id | 链路追踪 |
| player / op / amount / currency | 操作主体 |
| reason / source | 审计必需（如 `quest_reward` / `shop` / `gm`） |

**执行顺序**：校验参数 → 查幂等表 → 检查余额/背包空间 → 扣/加 → 写账本（TASK-030）→ 发布事件 → 返回。
**第一版不做**：拍卖行撮合、跨服交易、玩家间邮件附件（留给后续）。

## 9. Thread Model

经济操作由 SimulationThread 执行（单 Owner），账本写入异步投递到 Persistence 线程。禁止在 Tick 内同步等待账本落库（但**必须**在返回前完成内存态扣减与幂等标记）。

## 10. Hot Path

**NO**


## 11. External IO

**YES** （异步账本）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**NO**


## 13. Persistence

**YES**

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- server/gamenode/economy/include/mmo/game/economy/
- server/gamenode/economy/src/
- server/gamenode/economy/tests/
- config/gameplay/economy/

## 15. Implementation Steps

1. 定义 economy_command.h：EconomyCommand / EconomyOp / EconomyResult，字段按规范齐全
2. 定义 currency.h：CurrencyType 与余额表（per-player 定长数组）
3. 实现 price_table.h：价格表配置化（config/gameplay/economy/prices.json），禁止硬编码价格
4. 实现 economy_system.h/.cpp：Execute 八种操作，统一走 CommandBus 注册
5. 实现校验：余额不足 → 返回明确错误（不是静默扣成负数）；背包空间不足 → 拒绝且不扣钱
6. 实现 Transfer：两个玩家间的原子操作（同 Scene 内单线程原子；跨 Scene 第一版走 DataService 事务）
7. 实现 Purchase：查价格表 → 扣币 → 加物品，**两步必须同成功同失败**
8. 实现 Reward：任务/活动奖励，必带 reason 与 source
9. 实现事件：CurrencyChanged / ItemTraded / PurchaseCompleted，供任务系统与日志消费
10. 实现幂等占位：本地幂等表（key → result），为 TASK-030 的完整账本预留同接口
11. 写测试：八种操作的正常与异常路径；余额不足；背包满；并发转移；价格表缺失

## 16. Unit Test

八种操作各自的成功/失败路径；必填字段校验（缺 idempotency_key 拒绝）；余额边界（0、负数、超大值）；背包空间校验；Transfer 原子性；Purchase 两步一致性；事件产生

## 17. Integration Test

1000 玩家执行 1 万次混合经济操作（购买/奖励/转移/存取）：货币守恒（总发行量 - 总消耗 = 总余额）、无负数余额、无凭空产生物品、事件数量与操作一致；并发同一玩家操作串行化后结果正确

## 18. Benchmark

bin/economy_bench：`execute_ns=` / `balance_query_ns=` / `transfer_ns=` / `dedup_check_ns=`

## 19. Failure Test

余额不足：拒绝并返回明确错误码（禁止扣成负数）；背包满：拒绝购买且不扣钱；幂等键缺失：拒绝执行（这是硬性校验）；账本写入失败：内存态已改 → 进入重试队列，返回成功但标记 pending（或按配置返回失败并回滚，写死一种并测试）；重复提交同一幂等键：第二次返回首次结果（deduplicated=true）

## 20. Acceptance Criteria

1. **所有经济操作都经过 EconomyCommand**（grep：无直接改余额的代码路径）
2. 八种操作（Add/RemoveCurrency、Add/RemoveItem、Transfer、Purchase、Reward、Refund）全部实现
3. 余额不足/背包满时拒绝且不产生负余额或丢物品（单测）
4. 缺 idempotency_key 的命令被拒绝（单测）
5. 货币守恒（1 万次操作校验）
6. 价格表配置化（代码无硬编码价格）
7. Debug / Release 双构建通过，ctest -R Economy 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止任何绕过 EconomyCommand 直接改余额/物品的代码路径
- 禁止允许负余额
- 禁止缺 idempotency_key 的经济操作被执行
- 禁止硬编码价格（必须价格表配置化）
- 禁止在 Tick 内同步等待账本落库
- 禁止购买时扣钱成功但发物品失败（必须同成功同失败）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单次经济命令 < 2us（不含持久化）；余额查询 < 50ns；幂等检查 < 200ns；1 万次操作总耗时 < 100ms（不含 IO）。

## 23. Deliverables

- server/gamenode/economy/include/mmo/game/economy/economy_command.h
- server/gamenode/economy/include/mmo/game/economy/economy_system.h
- server/gamenode/economy/src/*.cpp
- server/gamenode/economy/tests/*
- config/gameplay/economy/prices.json
- server/gamenode/economy/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-029.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-029.sh
bash scripts/verify/task-029.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 017 026`
2. 交付物存在性检查（4 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Economy`
5. Benchmark 执行：`bin/economy_bench --ops 10000`
6. 性能阈值断言：`bench/economy.txt` 中 `execute_ns` ≤ `2000`
7. 性能阈值断言：`bench/economy.txt` 中 `balance_query_ns` ≤ `50`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-029

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Economy System

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-029.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-029
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
3. 查依赖：确认 TASK-017, TASK-026 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-029.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-017` · `server/gamenode/inventory`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
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
