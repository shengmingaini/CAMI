---
TASK-ID: TASK-022
NAME: Damage / Heal
PHASE: Phase 5 · 战斗
MODULE: server/gamenode/combat
OWNER: Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证
STATUS: DONE
DONE-DATE: 2026-09-04
DEPENDENCIES: TASK-011, TASK-016, TASK-021
---

# TASK-022 · Damage / Heal

> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。
> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`

| 字段 | 值 |
|---|---|
| TASK-ID | `TASK-022` |
| NAME | Damage / Heal |
| PHASE | Phase 5 · 战斗 |
| MODULE | `server/gamenode/combat` |
| OWNER | Codex / WorkBuddy Agent 实施；@技术总监 二次验收；本地 MinGW MSYS2 g++ + vcpkg 编译验证 |
| STATUS | **DONE** |
| DEPENDENCIES | `TASK-011`, `TASK-016`, `TASK-021` |

---

## 1. Objective

统一伤害与治疗：DamageEvent / HealEvent / DamageResult，支持物理、法术、暴击、抗性、护盾。**战斗热路径只能是内存、CPU 与本地接口，不得访问 MySQL / Redis / Kafka / 同步 gRPC。**

## 2. Dependencies

### 2.1 前置任务

- `TASK-011` · Entity System
- `TASK-016` · Player / Character
- `TASK-021` · Skill System

### 2.2 门禁规则

验收脚本会先执行 `require_tasks_done 011 016 021`：
任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。

## 3. Module

`server/gamenode/combat`

## 4. State Owner（状态归属）

HP 变化（伤害与治疗）的最终结果由 Damage / Heal 模块在 Combat 阶段独占写入。随机数必须来自确定性 PRNG（以 SceneID + TickNumber + 序列号为种子），禁止使用全局随机源，否则无法回放与对账。伤害公式的全部参数必须是配置数据，禁止硬编码在 C++ 里。

> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。
> 跨模块写入必须走 Command，禁止直接改对方内存。

## 5. Input

TASK-021 技能效果触发；TASK-016 属性（攻击/防御/暴击）

## 6. Output

damage 模块（伤害/治疗结算）+ 公式测试 + 热路径红线扫描

## 7. Public Interface

```cpp
namespace mmo::game::combat {
enum class DamageSchool : uint8_t { Physical, Magical, TrueDamage };
struct DamageRequest { entity::EntityId source; entity::EntityId target; DamageSchool school;
                       int64_t base_amount; float coefficient; bool can_crit{true};
                       bool can_be_dodged{true}; core::TraceID trace; core::RequestID request_id; };
struct DamageResult { int64_t raw; int64_t mitigated; int64_t absorbed; int64_t final_amount;
                      bool is_crit{false}; bool is_dodged{false}; bool is_blocked{false};
                      int64_t remaining_hp; bool lethal{false}; };
struct DamageEvent { entity::EntityId source; entity::EntityId target; DamageSchool school;
                     DamageResult result; uint64_t tick_number; core::TraceID trace; };
struct HealRequest { entity::EntityId source; entity::EntityId target; int64_t base_amount;
                     float coefficient; bool can_crit{true}; bool overheal_allowed{false};
                     core::TraceID trace; };
struct HealResult { int64_t raw; int64_t effective; int64_t overheal; int64_t remaining_hp; bool is_crit; };
class DamageSystem { public:
  DamageResult ComputeDamage(const DamageRequest&, const AttributeSet& attacker,
                             const AttributeSet& defender) const noexcept;   // 纯函数，无副作用
  core::Result<DamageResult> ApplyDamage(const DamageRequest&, const scene::SceneContext&);
  core::Result<HealResult>  ApplyHeal(const HealRequest&, const scene::SceneContext&);
  DamageStats Stats() const noexcept;   // 暴击率、闪避率、平均伤害——平衡性分析用
};
}
```

## 8. Data Model

**伤害公式（第一版，全部参数配置化）**

```
raw       = base_amount + coefficient * AttackPower
crit      = rand() < CritRate                      -> raw *= CritDamage(默认 1.5)
dodge     = rand() < DodgeRate                     -> final = 0
mitigated = raw * (1 - Defense / (Defense + K))    K 默认 400（随等级配置）
absorbed  = min(mitigated, ShieldValue)            -> 先扣护盾
final     = mitigated - absorbed
```

**结算顺序（固定，禁止随意调整）**：暴击判定 → 闪避判定 → 抗性减免 → 护盾吸收 → 扣 HP → 致死判定 → 发布事件。
**随机数**：使用 per-Scene 的确定性 PRNG（便于复现与回放），**禁止**用全局 rand()。

## 9. Thread Model

伤害计算是纯函数（ComputeDamage），可在任意线程调用；ApplyDamage 修改状态，只在 Scene 的 SimulationThread 的 Combat 阶段执行。

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

- server/gamenode/combat/include/mmo/game/combat/damage/
- server/gamenode/combat/src/damage/
- server/gamenode/combat/tests/
- config/gameplay/combat/

## 15. Implementation Steps

1. 定义 damage.h：DamageRequest / DamageResult / DamageEvent / HealRequest / HealResult
2. 实现 damage_formula.h：公式常量全部来自配置（config/gameplay/combat/formula.json），纯函数无随机（随机由调用方传入）
3. 实现 prng.h：per-Scene 确定性 PRNG（xorshift128+），支持种子设置与状态保存（便于回放）
4. 实现 damage_system.h/.cpp：ComputeDamage（纯函数）/ ApplyDamage（改状态 + 发事件）/ ApplyHeal
5. 实现护盾：先扣护盾再扣 HP，护盾值来自 Buff（TASK-023），本任务只定义接口
6. 实现致死判定：HP ≤ 0 → lethal=true + 发布 EntityDied 事件，HP 钳制为 0
7. 实现统计：暴击率、闪避率、平均伤害、总伤害量（平衡性分析必需）
8. 实现伤害日志（采样）：每秒采样 N 条伤害记录写入日志（禁止每条都写，防止日志打爆）
9. 写测试：公式各分支（暴击/闪避/抗性/护盾/致死）；边界（0 伤害、超高防御、护盾全覆盖、HP 恰好归零）；确定性 PRNG（同种子同结果）；统计正确性
10. 写**热路径红线测试**：用链接期拦截（mock 掉 mysql/redis/grpc 符号）或在 CI 用 nm 检查 damage 目标文件不引用这些符号，断言战斗结算不依赖外部 IO

## 16. Unit Test

伤害公式全分支与边界；护盾吸收顺序；致死判定与事件；治疗溢出；确定性 PRNG 可复现；统计聚合正确

## 17. Integration Test

5 人小队 vs 20 怪战斗 10000 次伤害结算：HP 守恒（总扣血 == 总 final_amount）、无负值、死亡事件数与致死次数一致、统计指标与逐条重算一致；热路径不触碰任何外部 IO（nm/链接检查）

## 18. Benchmark

bin/damage_bench：`compute_damage_ns=` / `apply_damage_ns=` / `damage_per_1k_ns=` / `alloc_per_damage=`

## 19. Failure Test

目标已死亡仍收到伤害：忽略并返回 NOT_FOUND（不产生负 HP）；护盾与 HP 同时归零：正确钳制且只发一次死亡事件；超高伤害（int64 上溢风险）：钳制到 MaxHp 相关上限，禁止溢出；NaN 系数：拒绝并返回 INVALID_ARGUMENT；PRNG 状态保存/恢复后序列连续（回放验证）

## 20. Acceptance Criteria

1. **伤害公式全部参数配置化**（grep 代码无硬编码系数）
2. 结算顺序固定：暴击 → 闪避 → 抗性 → 护盾 → 扣血 → 致死 → 事件（单测断言顺序）
3. **战斗结算不引用 MySQL / Redis / Kafka / gRPC**（链接符号检查通过）
4. 确定性 PRNG：同种子产生同结果（单测）
5. HP 守恒，无负值、无溢出（10000 次结算校验）
6. 采样日志生效，不每条全写（日志量可测）
7. Debug / Release 双构建通过，ctest -R Damage 全绿

以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。

## 21. Forbidden

- 禁止在伤害结算路径访问 MySQL / Redis / Kafka / 同步 gRPC / 文件 IO
- 禁止硬编码伤害公式系数（必须配置化）
- 禁止使用全局 rand()（必须 per-Scene 确定性 PRNG）
- 禁止产生负 HP 或整数溢出
- 禁止每条伤害都写日志（必须采样）
- 禁止伤害结算产生堆分配（alloc_per_damage = 0）

> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。

## 22. Performance Expectation

ComputeDamage < 50ns（纯计算）；ApplyDamage < 200ns；单次伤害分配次数 = 0；1000 次伤害结算 < 200us。

## 23. Deliverables

- server/gamenode/combat/include/mmo/game/combat/damage/damage.h
- server/gamenode/combat/include/mmo/game/combat/damage/damage_system.h
- server/gamenode/combat/include/mmo/game/combat/damage/prng.h
- server/gamenode/combat/src/damage/*.cpp
- server/gamenode/combat/tests/*
- config/gameplay/combat/formula.json
- server/gamenode/combat/docs/INTERFACE.md

## 24. Verification Script（本地验收）

**验收脚本**：`scripts/verify/task-022.sh`（由生成器产出，禁止手工编辑）

```bash
# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-022.sh
bash scripts/verify/task-022.sh
```

脚本执行的检查项：

1. 前置任务门禁：`require_tasks_done 011 016 021`
2. 交付物存在性检查（5 项）
3. 静态红线扫描：`server/gamenode/combat/src/damage` 内禁止出现 /(mysql|redis|grpc|kafka|sql::|std::ifstream|std::ofstream)/
4. CMake configure + 编译（Debug + Release 双构建）
5. ctest 过滤执行：`-R Damage`
6. Benchmark 执行：`bin/damage_bench --iterations 1000000`
7. 性能阈值断言：`bench/damage.txt` 中 `compute_damage_ns` ≤ `50`
8. 性能阈值断言：`bench/damage.txt` 中 `alloc_per_damage` ≤ `0`

脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。
脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。

## 25. Git Commit

**必须先通过验收脚本（退出码 0），才允许提交。**

```bash
# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）
bash scripts/task-done.sh TASK-022

# 2) 提交：Conventional Commits，scope 用模块名
git add -A
git commit -F - <<'EOF'
feat(server): Damage / Heal

- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）
- 实测数字：（粘贴 scripts/verify/task-022.sh 的真实输出，禁止写「性能良好」）

Refs: TASK-022
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
3. 查依赖：确认 TASK-011, TASK-016, TASK-021 均已 `STATUS: DONE`，否则停止并报告。
4. 查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。
5. 守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。
6. 做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。
7. 本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。
8. 跑单测：§16 Unit Test 全绿，新增代码必须带测试。
9. 跑集成：§17 Integration Test 全绿。
10. 跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。
11. 出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。
12. 跑验收脚本：`bash scripts/verify/task-022.sh` 退出码 0 后，才执行 §25 提交。

> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。

## 27. 接口契约、模块边界与扩展性

本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。

### 27.1 本任务导出的接口（冻结后不可破坏性变更）

见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` 即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。

### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）

- `TASK-011` · `server/gamenode/entity`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-016` · `server/gamenode/role`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`
- `TASK-021` · `server/gamenode/combat`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），禁止 `#include` 其 `src/`

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
