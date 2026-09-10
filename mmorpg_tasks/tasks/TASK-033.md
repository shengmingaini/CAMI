---
TASK-ID: TASK-033
NAME: Gameplay Script
PHASE: Phase 7 · Lua 脚本
MODULE: scripting/gameplay
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DEPENDENCIES: TASK-018, TASK-019, TASK-021, TASK-031, TASK-032
---

# TASK-033 · Gameplay Script

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-033` |
| NAME | Gameplay Script |
| PHASE | Phase 7 · Lua 脚本 |
| MODULE | `scripting/gameplay` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-018`, `TASK-019`, `TASK-021`, `TASK-031`, `TASK-032` |

---

## 1. Objective

用 Lua 实现第一版玩法脚本：Quest Script / Skill Formula / NPC AI / Boss Phase / Event Script。**这是验证「C++ Framework + Lua Rule」是否真正工作的关键任务。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-018` · NPC / Monster / AI
- `TASK-019` · Quest System
- `TASK-021` · Skill System
- `TASK-031` · Lua Runtime
- `TASK-032` · Lua Hot Reload

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 018 019 021 031 032`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`scripting/gameplay`

## 4. State Owner（状态归属）

脚本状态归属 ScriptContext（每 Scene）；业务状态（任务进度、Boss 阶段）归属对应 C++ 系统，脚本只能通过接口读写。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-031 运行时与绑定；TASK-032 热更；TASK-018/019/021 各 C++ 系统接口

## 6. Output

scripting/gameplay 脚本集 + C++/Lua 分工验证 + 热更实战演练

## 7. Public Interface

```lua
-- 契约：所有玩法脚本遵循统一入口（C++ 只认这四个函数）
function on_init(ctx)    end   -- 脚本加载
function on_event(ctx, name, payload) end   -- 事件回调
function on_tick(ctx, dt) end              -- 可选周期（低频，禁止高频）
function on_reload(ctx, old_version) end   -- 热更后的状态迁移
```

**C++ / Lua 分工红线**

| 能力 | 归属 | 理由 |
|---|---|---|
| Simulation / Entity / Memory / Scheduler / Network / AOI / 核心战斗框架 | **C++** | 性能与确定性 |
| Quest 规则 / Skill 公式 / Buff 公式 / NPC AI / Boss 阶段 / 活动脚本 | **Lua** | 迭代频率高 |
| 位置积分、伤害数值结算、AOI 计算 | **C++** | 禁止下放到 Lua |

## 8. Data Model

**第一版脚本清单（至少 5 类 × 各 3 个）**

| 类别 | 脚本 | 说明 |
|---|---|---|
| Quest Script | `quest/kill_10_wolves.lua` 等 | 监听 MonsterKilled 更新进度 |
| Skill Formula | `skill/fireball.lua` 等 | 计算 base + coeff × AP，返回给 C++ 结算 |
| NPC AI | `ai/aggressive_guard.lua` 等 | 状态转移钩子（Idle/Patrol/Chase 决策） |
| Boss Phase | `boss/dragon_phase.lua` 等 | 按 HP 百分比切阶段，切阶段时召唤/换技能 |
| Event Script | `event/double_exp.lua` 等 | 限时活动：经验翻倍，活动结束自动失效 |

**配置化**：脚本路径与绑定关系写在 `config/gameplay/scripts.json`，禁止硬编码到 C++。

## 9. Thread Model

脚本在所属 Scene 的 SimulationThread 执行（同步调用）。周期脚本（on_tick）默认 1Hz，禁止高频调用；AI 决策复用 TASK-018 的 5Hz 节流。

## 10. Hot Path

**YES** （技能公式在战斗热路径被调用）

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- scripting/gameplay/quest/
- scripting/gameplay/skill/
- scripting/gameplay/ai/
- scripting/gameplay/boss/
- scripting/gameplay/event/
- scripting/gameplay/tests/
- config/gameplay/scripts.json
- docs/

## 15. Implementation Steps

1. 定义脚本契约：四入口函数 + ctx 结构（Entity/Skill/Quest/Event/Query API）
2. 实现 C++ 侧脚本加载器：按 config/gameplay/scripts.json 绑定「系统钩子 ↔ 脚本」
3. 编写 3 个 Quest Script：监听事件更新进度、多目标、限时任务
4. 编写 3 个 Skill Formula：返回伤害/治疗公式结果（C++ 仍负责最终结算与随机）
5. 编写 3 个 NPC AI 脚本：巡逻/警戒/逃跑三种行为钩子（复用 C++ 状态机，Lua 只做决策）
6. 编写 3 个 Boss Phase 脚本：按 HP 阈值切阶段、切换技能组、召唤小怪、阶段广播
7. 编写 3 个 Event Script：限时双倍经验、世界事件、节日活动（含自动失效）
8. 实现脚本级单元测试框架（Lua 侧）：可 mock ctx 并断言行为
9. 实现热更实战：用 TASK-032 的 scriptctl 热更一个技能公式，验证线上即时生效且可回滚
10. 写**分工验证测试**：脚本试图改 HP 数值 / 做 IO → 被拦截（证明红线有效）

## 16. Unit Test

15 个脚本各自的 Lua 侧单测（mock ctx）；脚本加载与绑定；四入口函数齐全；脚本违反红线被拦截；配置驱动加载

## 17. Integration Test

**端到端玩法验证**：玩家接任务 → 杀怪（C++ 发事件）→ Lua 任务脚本更新进度 → 完成任务 → Lua 奖励脚本发奖 → 玩家用 Lua 公式技能打 Boss → Boss 血量到 70% → Lua 阶段脚本切阶段 → 全员收到广播。全链路跑通，且每一步的状态归属正确

## 18. Benchmark

bin/gameplay_script_bench：`skill_formula_ns=` / `quest_script_ns=` / `boss_phase_check_ns=` / `script_total_cpu_percent=`

## 19. Failure Test

脚本语法错误：加载失败并回退到 C++ 默认行为（禁止整个系统崩）；脚本超时：指令上限拦截，本次调用作废；脚本返回非法值（NaN/负伤害）：C++ 侧校验并钳制；热更后脚本状态丢失：on_reload 正确迁移；脚本引用的 Quest/Boss 不存在：加载期校验报错

## 20. Acceptance Criteria

1. 五类脚本（Quest / Skill Formula / NPC AI / Boss Phase / Event）各至少 3 个，共 ≥ 15 个
2. **端到端玩法链路跑通**（集成测试，见上）
3. 脚本路径配置化，C++ 无硬编码脚本名
4. 脚本违反红线（改 HP / 做 IO）被拦截（单测）
5. 热更一个技能公式，线上即时生效且可回滚（实战演练）
6. 脚本总 CPU 占比 < 10%（benchmark 实测，禁止把所有计算塞进 Lua）
7. Debug / Release 双构建通过，ctest -R GameplayScript 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止把位置积分 / AOI / 伤害最终结算下放到 Lua
- 禁止脚本直接改 HP、伤害数值等实时状态（必须走系统接口）
- 禁止脚本做文件 / 网络 / 数据库 IO
- 禁止高频（> 1Hz）调用周期脚本
- 禁止硬编码脚本路径
- 禁止脚本错误导致整个 Scene 崩溃
- 禁止脚本总 CPU 占比超过 10%

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单次技能公式调用 < 3us；单次任务脚本处理 < 2us；Boss 阶段检查 < 1us；脚本总 CPU 占比 < 10%；15 个脚本加载 < 50ms。

## 23. Deliverables

- scripting/gameplay/quest/*.lua
- scripting/gameplay/skill/*.lua
- scripting/gameplay/ai/*.lua
- scripting/gameplay/boss/*.lua
- scripting/gameplay/event/*.lua
- scripting/gameplay/tests/*
- config/gameplay/scripts.json
- scripting/gameplay/docs/README.md
- docs/gameplay-script-report.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-033.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-033.sh
bash scripts/verify/task-033.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 018 019 021 031 032`
2. 交付物存在性检查（4 项）
3. CMake configure + 编译（Debug + Release 双构建）
4. ctest 过滤执行：`-R GameplayScript`
5. Benchmark 执行：`bin/gameplay_script_bench --iterations 100000`
6. 性能阈值断言：`bench/gameplay.txt` 中 `skill_formula_ns` ≤ `3000`
7. 性能阈值断言：`bench/gameplay.txt` 中 `script_total_cpu_percent` ≤ `10`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-033

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(script): Gameplay Script

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-033.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-033
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
3. 查依赖：确认 TASK-018, TASK-019, TASK-021, TASK-031, TASK-032 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-033.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-018` · `server/gamenode/ai`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-019` · `server/gamenode/quest`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-021` · `server/gamenode/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-031` · `scripting/lua`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-032` · `scripting/lua`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
