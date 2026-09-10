---
TASK-ID: TASK-041
NAME: 跨进程集成与战斗性能回归
PHASE: Phase 8 · 集成与回归
MODULE: tools/qa
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-025, TASK-030, TASK-033
---

# TASK-041 · 跨进程集成与战斗性能回归

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-041` |
| NAME | 跨进程集成与战斗性能回归 |
| PHASE | Phase 8 · 集成与回归 |
| MODULE | `tools/qa` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-025`, `TASK-030`, `TASK-033` |

---

## 1. Objective

补齐「跨进程全链路」与「Lua 落地后的战斗性能回归」两块验证缺口：① 跑一次**真实 gRPC + Redis + MySQL** 的端到端演练（Login→Gateway→GameNode→Scene→战斗→经济→持久化）；② 在 TASK-033（Lua 玩法脚本，运行在 Combat Tick 内）落地后，**重跑 TASK-025 的 1k 战斗性能矩阵**，防止 Lua 拖垮 Tick。本任务不拥有任何服务状态，是纯验证方，禁止改动被验证模块的实现。

## 2. Dependencies

### 2.1 前置任务

- `TASK-025` · Combat Benchmark（架构可行性判定点）
- `TASK-030` · Economic Ledger / Idempotency
- `TASK-033` · Gameplay Script

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 025 030 033`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`tools/qa`

## 4. State Owner（状态归属）

集成与回归任务不拥有任何服务状态，是纯验证方。它消费 TASK-025 的战斗性能矩阵定义、TASK-030 的账本对账、TASK-033 的 Lua 脚本全集，做跨进程（gRPC + Redis + MySQL）端到端演练与「Lua 落地后的战斗性能回归」。禁止在回归中改动被验证模块的实现。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-025 战斗性能矩阵定义（五场景×四规模）；TASK-030 账本对账（economy_audit.py）；TASK-033 Lua 脚本全集（boss/event/quest/skill/npc）

## 6. Output

跨进程端到端测试套件 + 战斗性能回归报告（Lua 前后对比）+ 回归门禁脚本

## 7. Public Interface

```cpp
// 无新增业务接口；消费既有接口：
//  - TASK-025 combat_benchmark：重跑矩阵，输出 tick_p95_us/phase_p95_us
//  - TASK-030 economy_audit：对账，断言五场景故障下资金守恒
//  - TASK-005 协议：驱动真实 Bot 走完整链路
namespace mmo::qa {
class E2EScenario { public:
  core::Result<void> Run(const E2EConfig&);   // 启 Gateway+GameNode+DataService，跑全流程
  RegressionReport Report() const; };         // Lua 前后 Tick 对比
}
```

## 8. Data Model

**回归矩阵（必须全跑，禁止抽样）**

| 项 | 内容 |
|---|---|
| 跨进程 E2E | Bot 真实走 Login→Gateway→GameNode→EnterScene→Attack→Loot→Trade→Persist；断言各阶段可观测 |
| 账本对账 | 复用 TASK-030 economy_audit.py，五场景故障下资金守恒（不重复扣/发/复制） |
| 战斗回归 | 在 TASK-033 Lua 全量加载后，重跑 TASK-025 的 1000 玩家 100% Combat 矩阵，对比 Lua 前基线 |

**判定**：Lua 后 tick_p95_us 仍 ≤ 5000、tick_p99_us 仍 ≤ 8000；任一退化 > 5% 即判回归失败并记录瓶颈（通常是某 Lua 脚本耗时）。

## 9. Thread Model

本任务测试进程独立于服务进程；可指定多机部署以模拟真实跨进程。

## 10. Hot Path

**YES** （回归对象即战斗热路径）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**YES** （写报告）

所有外部 IO 必须异步化，禁止出现在 Tick 内。

## 12. Network RPC

**YES** （驱动真实 gRPC）

跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。

## 13. Persistence

**YES** （触发真实持久化）

持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。

## 14. Files

- tools/qa/e2e/
- tools/qa/regression/
- tools/qa/docs/REGRESSION.md

## 15. Implementation Steps

1. 搭建跨进程 E2E 编排：本地起 Gateway + GameNode + DataService（Redis+MySQL），用真实 Bot 走完整链路
2. 实现 E2E 断言：每阶段可观测（Session 七字段、Scene 创建、战斗伤害、掉落入库、交易对账）
3. 接入 TASK-030 economy_audit.py：五场景故障下资金守恒断言
4. 在 TASK-033 Lua 全量加载后，调用 TASK-025 的 combat_benchmark 重跑 1k 100% Combat 矩阵
5. 产出回归报告：Lua 前/后 tick_p95/p99 对比，退化 > 5% 标红并定位耗时 Lua 脚本
6. 写 REGRESSION.md：回归门禁定义（阈值、频率建议：每次 Lua/战斗改动后跑）
7. 把回归挂为可重复命令：`bash tools/qa/regression/run.sh`

## 16. Unit Test

E2E 编排器能正确拉起/销毁服务进程；回归报告解析与阈值判定正确

## 17. Integration Test

完整 E2E 一次跑通（含一次注入的 GameNode 崩溃，验证 TASK-037 重连链路在真实跨进程下可用）；账本对账五场景全过

## 18. Benchmark

复用 TASK-025 combat_benchmark（Lua 后重跑），输出 tick_p95_us/tick_p99_us/phase_p95_us

## 19. Failure Test

回归退化 > 5%：报告标红并定位 Lua 脚本，不静默通过；E2E 中某服务启动失败：明确报错并清理残留进程，不留下僵尸

## 20. Acceptance Criteria

1. 跨进程 E2E 一次完整跑通（Login→…→Persist），每阶段断言可观测
2. TASK-030 economy_audit 五场景故障下资金守恒全过
3. TASK-033 Lua 全量加载后重跑 TASK-025 1k 战斗矩阵：tick_p95 ≤ 5000、tick_p99 ≤ 8000，退化 < 5%
4. 回归报告 REGRESSION.md 含 Lua 前/后对比与瓶颈定位
5. 回归命令可重复执行，作为后续 Lua/战斗改动的常驻门禁
6. Debug / Release 双构建通过，ctest -R QA_E2E 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在回归中改动被验证模块（TASK-025/030/033）的实现
- 禁止抽样跑矩阵（必须 5 场景×4 规模 + Lua 前后两次）
- 禁止在退化 > 5% 时静默通过
- 禁止 E2E 残留僵尸进程（必须清理）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

E2E 单轮 < 5min（含启动）；Lua 后 1k 100% Combat：tick_p95 ≤ 5000us、tick_p99 ≤ 8000us，与 Lua 前基线退化 < 5%。

## 23. Deliverables

- tools/qa/e2e/e2e_runner.h
- tools/qa/regression/run.sh
- tools/qa/docs/REGRESSION.md
- bench/combat_regression_lua.txt

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-041.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-041.sh
bash scripts/verify/task-041.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 025 030 033`
2. 交付物存在性检查（4 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R QA_E2E`
5. Benchmark 执行：`bin/combat_bench --matrix --lua-loaded --duration 60 --warmup 5 --out bench/combat_regression_lua.json`
6. 性能阈值断言：`bench/combat_regression_lua.txt` 中 `tick_p95_us` ≤ `5000`
7. 性能阈值断言：`bench/combat_regression_lua.txt` 中 `tick_p99_us` ≤ `8000`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-041

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(tools): 跨进程集成与战斗性能回归

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-041.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-041
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
3. 查依赖：确认 TASK-025, TASK-030, TASK-033 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-041.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-025` · `benchmark/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-030` · `server/gamenode/economy`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-033` · `scripting/gameplay`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
