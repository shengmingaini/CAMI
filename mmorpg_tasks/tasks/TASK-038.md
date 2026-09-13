---
TASK-ID: TASK-038
NAME: Bot + Load + Chaos + Delivery
PHASE: Phase 10 · 最终工程验收
MODULE: tools/qa + docs/architecture
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-025, TASK-027, TASK-028, TASK-037, TASK-039, TASK-040, TASK-041
---

# TASK-038 · Bot + Load + Chaos + Delivery

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-038` |
| NAME | Bot + Load + Chaos + Delivery |
| PHASE | Phase 10 · 最终工程验收 |
| MODULE | `tools/qa + docs/architecture` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-025`, `TASK-027`, `TASK-028`, `TASK-037`, `TASK-039`, `TASK-040`, `TASK-041` |

---

## 1. Objective

最终工程验证平台：Bot 框架、逐级 CCU 压测、网络模拟、Chaos 测试、最终交付包。**这是整个项目能否交付的判定点。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-025` · Combat Benchmark（架构可行性判定点）
- `TASK-027` · Redis Adapter
- `TASK-028` · MySQL Adapter
- `TASK-034` · Client Core（RFC §9.8 已释放依赖：Bot 为协议层客户端，复用 TASK-005，不依赖客户端子树）
- `TASK-036` · Resource / Low Spec System（RFC §9.8 已释放依赖，同上）
- `TASK-037` · Reconnect / Failover / Scene Recovery
- `TASK-039` · Social System（组队/好友/公会/聊天/邮件）
- `TASK-040` · ControlService（控制面：节点管理/配置下发/健康/运维）
- `TASK-041` · 跨进程集成与战斗性能回归

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 025 027 028 037 039 040 041`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`tools/qa + docs/architecture`

## 4. State Owner（状态归属）

Bot 自身状态由各 Bot 实例拥有（无共享）；压测数据汇总到独立的采集器进程。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-025 战斗基准；TASK-034/036 客户端；TASK-037 容灾；TASK-027/028 数据服务；TASK-039 Social 运行时；TASK-040 ControlService 控制面；TASK-041 跨进程集成与战斗回归

## 6. Output

Bot 框架 + 压测套件 + Chaos 套件 + 最终交付包（架构文档/源码/docker/k8s/schema/proto/lua/tests/benchmarks/tools/docs）

## 7. Public Interface

```cpp
// 38.1 Bot Framework（无渲染，纯协议层）
namespace mmo::bot {
enum class BotAction : uint8_t { Login, Move, Attack, Quest, Trade, Chat, Logout, Reconnect };
struct BotScript { std::vector<BotAction> actions; std::vector<DurationMs> delays; uint32_t loop{1}; };
class Bot { public:
  core::Result<void> Run(const BotScript&, std::string_view gateway_addr);
  BotStats Stats() const noexcept; };        // actions_done / errors / rtt_ms_p95 / received_pps
class BotFarm { public:                      // 单机启动 N 个 Bot
  core::Result<void> Spawn(uint32_t count, const BotConfig&);
  core::Result<AggregateStats> RunUntil(DurationMs);
  core::Result<void> StopAll(DurationMs grace); };
}
// 38.2~38.4 由 tools/ 下的脚本编排，非 C++ 接口
```

## 8. Data Model

**38.2 CCU 阶梯（必须逐级跑完，禁止跳级）**

| 级别 | 通过条件 |
|---|---|
| 100 | Tick P99 ≤ 8ms、错误率 < 0.1% |
| 500 | 同上 |
| 1000 | 同上 |
| 5000 | 同上 + 单 GameNode 承受 |
| 10000 | 多 GameNode 水平扩容 |
| 20000 | 水平扩容 + 数据层无瓶颈 |
| 50000 | 最终目标；**不达标则记录容量上限，不宣称达成** |

**38.3 网络模拟**：Latency(50/100/200ms) / Jitter(±20ms) / Packet Loss(1%/5%) / Bandwidth Limit / Disconnect / Reconnect。
**38.4 Chaos**：GameNode Crash / Gateway Crash / Redis Failure / MySQL Failure / Network Partition / CPU Saturation / Memory Pressure —— **七项全部真跑**。
**38.5 交付清单**：architecture/{architecture.md, state-ownership.md, dependency.md, sequence/, deployment/, capacity-report.md} + source / docker / k8s / schema / proto / flatbuffers / lua / tests / benchmarks / tools / docs。

## 9. Thread Model

Bot 进程独立于服务进程，可多机部署；每个 Bot 单线程事件驱动（支持单机数千连接）。压测期间禁止在被测机器上跑 Bot（会污染数据）。

## 10. Hot Path

**NO** （测试工具）


## 11. External IO

**YES** （写报告）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES** （压测流量）

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**NO**


## 14. Files

- tools/bot/
- tools/load/
- tools/chaos/
- tools/netsim/
- tools/report/
- docs/architecture/
- deploy/docker/
- deploy/k8s/

## 15. Implementation Steps

1. **38.1** 实现 Bot：协议层客户端（复用 TASK-005），八种行为（Login/Move/Attack/Quest/Trade/Chat/Logout/Reconnect）
2. **38.1** 实现 BotFarm：单机启动 N 个 Bot，资源占用可控（目标单机 5000 Bot）
3. **38.2** 实现压测编排 tools/load/run_ladder.sh：按 100/500/1000/5000/10000/20000/50000 逐级，采集 CPU/RAM/Network/Tick/P95/P99/Redis OPS/MySQL QPS/Error Rate
4. **38.2** 实现指标采集器：从各进程拉取指标，统一写入时序文件（CSV/JSON）
5. **38.3** 实现网络模拟：接入 tc/netsh 或代理层，支持延迟/抖动/丢包/带宽/断连/重连六种
6. **38.4** 实现 Chaos 套件 tools/chaos/：七种故障注入脚本（kill 进程 / 停容器 / 网络分区 / CPU 打满 / 内存压力）
7. **38.4** 每个 Chaos 用例必须有**明确的预期行为与恢复断言**（不是“看看会不会崩“）
8. **38.5** 编写 docs/architecture/ 六份文档：architecture.md / state-ownership.md / dependency.md / sequence/ / deployment/ / capacity-report.md
9. **38.5** 整理交付包：docker-compose、k8s manifests、schema（SQL 迁移）、proto、flatbuffers、lua 脚本、tests、benchmarks、tools、docs
10. **38.5** 编写 README / DEPLOYMENT / OPERATIONS 三份运维文档
11. 跑完整阶梯压测与七项 Chaos，生成最终报告

## 16. Unit Test

Bot 八种行为各自可用；BotScript 解析与循环；BotFarm 批量启停；指标采集字段完整；报告生成格式正确

## 17. Integration Test

**最终验收（全部真跑，禁止模拟代替）**

1. CCU 阶梯 7 级全部跑完，每级数据完整（未达标要记录实际容量上限）
2. 七项 Chaos 全部执行，每项有恢复断言与耗时
3. 网络模拟六种场景，客户端表现符合预期（不外挂、不崩溃、可重连）
4. 交付包可在一台新机器上 `docker compose up` 后跑通 100 CCU 冒烟

## 18. Benchmark

bin/bot_bench + tools/load/：`ccu_level=` / `tick_p95_ms=` / `tick_p99_ms=` / `cpu_percent=` / `rss_mb=` / `redis_ops=` / `mysql_qps=` / `error_rate=` / `net_mbps=`

## 19. Failure Test

压测中服务崩溃：记录崩溃点 CCU 与原因，该级判定失败（不得美化）；Chaos 后无法恢复：判定该用例失败并记录 RTO；网络极端丢包 5%：客户端应可玩（ degraded 但不断连），若断连则记录；容量不达 50000：如实记录实际容量上限与瓶颈（写入 capacity-report.md），**禁止宣称达成**

## 20. Acceptance Criteria

1. **CCU 阶梯 7 级全部跑完**（100/500/1000/5000/10000/20000/50000），数据完整
2. **七项 Chaos 全部执行**（GameNode Crash / Gateway Crash / Redis Failure / MySQL Failure / Network Partition / CPU Saturation / Memory Pressure），每项有恢复断言
3. 网络模拟六种场景全部覆盖
4. docs/architecture/ 六份文档齐全
5. 交付包在新机器上可复现 100 CCU 冒烟
6. capacity-report.md 如实记录容量上限（不达标不得宣称）
7. 全部历史 benchmark 数据汇总归档
8. Debug / Release 双构建通过，全量 ctest 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止跳级压测（必须逐级）
- 禁止用模拟代替真实故障演练
- 禁止在未达标时宣称支持 50000 CCU
- 禁止 Chaos 用例没有恢复断言
- 禁止在被测机器上跑 Bot（数据污染）
- 禁止交付包缺少架构六文档中的任何一份
- 禁止遗漏任何一项真实故障演练

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

逐级 CCU 全部记录；目标 50000 CCU 下 Tick P99 ≤ 8ms、错误率 < 0.1%。**若未达标，capacity-report.md 必须写明实际容量上限、瓶颈组件与扩容建议，禁止宣称达成。**

## 23. Deliverables

- tools/bot/*
- tools/load/*
- tools/chaos/*
- tools/netsim/*
- tools/report/*
- docs/architecture/architecture.md
- docs/architecture/state-ownership.md
- docs/architecture/dependency.md
- docs/architecture/sequence/*
- docs/architecture/deployment/*
- docs/architecture/capacity-report.md
- deploy/docker/*
- deploy/k8s/*
- README.md
- DEPLOYMENT.md
- OPERATIONS.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-038.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-038.sh
bash scripts/verify/task-038.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 025 027 028 034 036 037 039 040 041`
2. 交付物存在性检查（8 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R Bot`
5. Benchmark 执行：`bin/bot_bench --bots 1000 --duration 300`
6. 性能阈值断言：`bench/load_1000.txt` 中 `tick_p99_ms` ≤ `8`
7. 性能阈值断言：`bench/load_1000.txt` 中 `error_rate` ≤ `0.001`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-038

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(tools): Bot + Load + Chaos + Delivery

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-038.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-038
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
3. 查依赖：确认 TASK-025, TASK-027, TASK-028, TASK-034, TASK-036, TASK-037, TASK-039, TASK-040, TASK-041 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-038.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-025` · `benchmark/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-027` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-028` · `server/dataservice`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-034` · `client/core`：RFC §9.8 已释放依赖——Bot 为协议层客户端，复用 TASK-005（mmo::protocol），不依赖客户端子树，故本任务不再消费其接口。
- `TASK-036` · `client/resource`：RFC §9.8 已释放依赖，同上。
- `TASK-037` · `server/gateway + server/gamenode`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-039` · `server/gamenode/social`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-040` · `server/control`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-041` · `tools/qa`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
| 2026-09-14 | 架构级释放（RFC §9.8）：解除 TASK-038 对 TASK-034 / TASK-036（Godot 客户端子树）的依赖。理由——Bot 为协议层客户端，复用 TASK-005（mmo::protocol）即可驱动 8 种行为，独立演进、不阻塞最终工程验收；同步更新 §2.1 / §2.2 / §27.2 与 scripts/verify/task-038.sh 的 `require_tasks_done`（去掉 034 036）。注：任务书源为 `tools/gen` 生成器产物，但该生成器当前缺失，故此处为必要的手工编辑并显式记录。 |
