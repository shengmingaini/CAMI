---
TASK-ID: TASK-023
NAME: Buff / Debuff
PHASE: Phase 5 · 战斗
MODULE: server/gamenode/combat
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: PENDING
DEPENDENCIES: TASK-004, TASK-016
---

# TASK-023 · Buff / Debuff

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-023` |
| NAME | Buff / Debuff |
| PHASE | Phase 5 · 战斗 |
| MODULE | `server/gamenode/combat` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **PENDING** |
| DEPENDENCIES | `TASK-004`, `TASK-016` |

---

## 1. Objective

实现 Buff 系统：BuffDefinition / BuffInstance / Duration / Stack / Tick / Remove。**必须使用 Scheduler，禁止每个 Buff 开一个 Timer 线程。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-004` · Core Memory / Thread / Scheduler
- `TASK-016` · Player / Character

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 004 016`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/combat`

## 4. State Owner（状态归属）

Buff 实例的 Owner 是 Buff System，且必须挂在 Scheduler 上按 Tick 结算，禁止自建线程或独立定时器。Buff 引起的属性 / HP 变化仍需经对应 Owner 的 Command 落地，Buff 不得直接改 HP。堆叠、刷新、驱散规则由 Buff 独占判定。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-004 Scheduler（Buff tick 驱动）；TASK-016 AttributeSet（from_buff 层）

## 6. Output

buff 模块（定义/实例/层数/周期/移除）+ 堆叠测试 + Buff 压力测试

## 7. Public Interface

```cpp
namespace mmo::game::combat {
using BuffId = uint32_t;
enum class BuffKind : uint8_t { Buff, Debuff, Dot, Hot, Shield, Stun, Root, Silence };
enum class StackRule : uint8_t { None, Refresh, Independent };   // 不可叠 / 刷新时长 / 独立层数
struct BuffDef { BuffId id; std::string_view name; BuffKind kind; StackRule stack_rule;
                 uint32_t max_stacks{1}; DurationMs duration{10000}; DurationMs tick_interval{0};
                 std::array<int64_t, kAttrCount> attr_modifiers;   // 加值
                 std::array<float, kAttrCount> attr_multipliers;   // 乘值（先加后乘）
                 int64_t shield_value{0}; EffectDef tick_effect; bool dispellable{true}; };
struct BuffInstance { BuffId def_id; uint32_t stacks{1}; core::SteadyTime expire_at;
                      core::SteadyTime next_tick_at; entity::EntityId source; uint32_t version{0}; };
class BuffSystem { public:
  core::Result<uint32_t> Apply(entity::EntityId target, BuffDef def, entity::EntityId source, core::TraceID);
  core::Result<void> Remove(entity::EntityId target, BuffId, RemoveReason, core::TraceID);
  core::Result<void> Dispel(entity::EntityId target, BuffKind kind, uint32_t count, core::TraceID);
  core::Result<void> Tick(const scene::SceneContext&);      // Buff 阶段驱动
  const std::vector<BuffInstance>* BuffsOf(entity::EntityId) const noexcept;
  size_t ActiveBuffCount() const noexcept;      // 指标：全 Scene Buff 总数
};
}
```

## 8. Data Model

**堆叠规则**

| 规则 | 行为 |
|---|---|
| None | 已存在则拒绝或刷新（按配置），层数恒为 1 |
| Refresh | 已存在则刷新 expire_at 与 stacks（stacks 不超 max_stacks） |
| Independent | 每次 Apply 独立计一层，各自计时 |

**属性计算顺序**：`final = (base + equipment) * (1 + Σ buff_multipliers) + Σ buff_adders`，**先加后乘**，禁止各 Buff 直接改 Final 值。
**Tick**：`tick_interval > 0` 的 Buff 在 Buff 阶段按 next_tick_at 触发效果（DOT/HOT），到期移除。

## 9. Thread Model

Buff 只在 Scene 的 SimulationThread 的 Buff 阶段处理，走 TASK-004 Scheduler 的 Tick 驱动（宿主驱动，非自带线程）。**严格禁止每 Buff 一个线程/一个 OS timer。**

## 10. Hot Path

**YES**

本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / 网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。

## 11. External IO

**NO**


## 12. Network RPC

**NO**


## 13. Persistence

**NO**


## 14. Files

- server/gamenode/combat/include/mmo/game/combat/buff/
- server/gamenode/combat/src/buff/
- server/gamenode/combat/tests/
- server/gamenode/combat/benchmark/
- config/gameplay/buffs/

## 15. Implementation Steps

1. 定义 buff_def.h：BuffDef / BuffInstance / BuffKind / StackRule，配置化（config/gameplay/buffs/*.json）
2. 实现 buff_container.h：per-Entity 的 Buff 数组（定长，默认 32 槽），禁止无界 vector 频繁扩容
3. 实现 buff_system.h/.cpp：Apply / Remove / Dispel / Tick
4. 实现三种堆叠规则：None / Refresh / Independent，各规则行为写死并测试
5. 实现 Tick 效果：到期的 DOT/HOT 触发 DamageRequest/HealRequest（复用 TASK-022，不重复实现伤害逻辑）
6. 实现属性联动：Buff 变更时更新 AttributeSet.from_buff 层并 Recompute（禁止直接改 Final）
7. 实现驱散：按 kind 批量驱散 N 个（优先级：Debuff 优先，可按配置）
8. 实现移除原因：Expired / Dispel / Death / Replace / Manual，每种发布对应事件
9. 写测试：三种堆叠规则；到期移除；Tick 触发；属性加乘顺序；驱散；控制类（Stun/Root/Silence）与移动/技能联动；Buff 槽位上限
10. 写压力测试：1000 实体 × 20 Buff = 2 万 Buff 同时存在，跑 10 分钟，验证 Buff 阶段耗时与内存

## 16. Unit Test

三种堆叠规则；到期与 Tick 时序；属性加乘顺序（先加后乘）；驱散优先级与数量；移除原因与事件；槽位上限；配置加载校验

## 17. Integration Test

1000 实体各带 20 个 Buff（含 DOT/HOT/护盾/眩晕）跑 10 分钟：DOT 每 Tick 正确扣血、护盾正确吸收、眩晕期间移动被拒、到期自动移除、属性联动正确（装备+Buff 叠加后 Total 值符合公式）、无泄漏

## 18. Benchmark

bin/buff_bench：`apply_ns=` / `tick_ns_per_buff=` / `buff_phase_us_at_20k=` / `mem_bytes_per_buff=` / `thread_count=`

## 19. Failure Test

Buff 槽位满：拒绝新 Buff 并返回 BUSY（禁止静默丢弃最旧的）；目标在 Buff Tick 时已死亡：跳过且不崩溃；移除不存在的 Buff：幂等返回成功；Buff 定义引用不存在的效果：加载期报错；DOT 致死：正确触发死亡事件且 Buff 随之移除（死亡清 Buff 顺序固定）

## 20. Acceptance Criteria

1. **Buff Tick 走 Scheduler，不创建任何线程**（grep 验证 + 运行时 `thread_count` 不随 Buff 数增长）
2. 三种堆叠规则全部实现且有单测
3. 属性计算先加后乘，Buff 只写 from_buff 层（代码评审 + 单测）
4. 2 万 Buff 同时存在，Buff 阶段耗时达标（benchmark 实测）
5. DOT/HOT/护盾/控制四类 Buff 均可工作（集成测试）
6. Buff 配置全部配置化
7. Debug / Release 双构建通过，ctest -R Buff 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止为每个 Buff 创建一个线程或一个 OS 定时器
- 禁止 Buff 直接修改 Final 属性值（必须走 from_buff 层 + Recompute）
- 禁止 Buff 容器无界增长（必须定长槽位）
- 禁止 Buff Tick 阶段做阻塞 IO 或同步 RPC
- 禁止硬编码 Buff 数值
- 禁止槽位满时静默丢弃（必须返回 BUSY + 指标）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

单个 Buff 内存 < 64B；Apply < 300ns；2 万 Buff 的 Buff 阶段 < 400us；Tick 处理 < 50ns/Buff；线程数恒定（不随 Buff 数增长）。

## 23. Deliverables

- server/gamenode/combat/include/mmo/game/combat/buff/buff_def.h
- server/gamenode/combat/include/mmo/game/combat/buff/buff_system.h
- server/gamenode/combat/src/buff/*.cpp
- server/gamenode/combat/tests/*
- server/gamenode/combat/benchmark/*
- config/gameplay/buffs/*.json
- server/gamenode/combat/docs/INTERFACE.md
- server/gamenode/combat/docs/PERFORMANCE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-023.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-023.sh
bash scripts/verify/task-023.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 004 016`
2. 交付物存在性检查（1 项）
3. 静态红线扫描：`server/gamenode/combat/src/buff` 内禁止出现 /std::thread/
4. 静态红线扫描：`server/gamenode/combat/src/buff` 内禁止出现 /(mysql|redis|grpc)/
5. CMake configure + 编译（Debug + Release 双构建）
6. ctest 过滤执行：`-R Buff`
7. Benchmark 执行：`bin/buff_bench --entities 1000 --buffs-per-entity 20 --ticks 12000`
8. 性能阈值断言：`bench/buff.txt` 中 `buff_phase_us_at_20k` ≤ `400`
9. 性能阈值断言：`bench/buff.txt` 中 `mem_bytes_per_buff` ≤ `64`
10. 性能阈值断言：`bench/buff.txt` 中 `thread_count_delta` ≤ `0`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-023

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Buff / Debuff

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-023.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-023
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
3. 查依赖：确认 TASK-004, TASK-016 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-023.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-004` · `engine/core`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-016` · `server/gamenode/role`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
