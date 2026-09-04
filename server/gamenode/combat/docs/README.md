# Combat 模块 · README

MMORPG 战斗模块（Phase 5 战斗）。

- **TASK-021 · Skill System**：技能定义 / 施法 / 冷却 / 目标选取（[下](#task-021--skill-system)）
- **TASK-022 · Damage / Heal**：统一伤害与治疗结算（[下](#task-022--damage--heal)）

---

# TASK-021 · Skill System

## 范围

- 四类目标：`Self` / `SingleTarget` / `AoeCircle` / `AoeCone` / `Projectile`
  （配置里 5 种 `target_type`，验收口径的四类 = 自身 / 单体 / AOE / 飞行物）。
- 施法生命周期：`TryCast`（校验 → 扣资源 → 读条或瞬发 → 结算）→ `Update`（读条完成 / 飞行物推进）。
- 读条状态机：`Idle → Casting → Idle`，可被 `InterruptCasting` 中断，**中断不退资源**（§15.7）。
- 四类校验：冷却（`OnCooldown`）、资源（`InsufficientResource`）、距离（`OutOfRange`）、
  目标（`NoTarget` / `InvalidTarget`），外加 `Silenced`；`CastResult` 各分支均有单测可达。
- AOE 目标选取走 **AOI 局部查询**（`IAoi::QueryVisible`），禁止全 Scene 扫描（§21 Forbidden）。
- 冷却追踪 `CooldownTracker`：每实体每技能一张时间戳数组，查询 **O(1)**，热路径无 map 查找。
- 技能 / Buff 全部配置化：`config/gameplay/skills/*.json`（12 条技能）+ `config/gameplay/buffs/buffs.json`。

## 不在本任务范围

- Buff 实际生效（仅发布 `BuffApplied` 事件，留待 TASK-023）。
- 伤害管线（暴击 / 护甲 / 仇恨）：由 TASK-022 的 DamageSystem 承担，
  本任务仍保留自己的基础结算路径（Skill 侧未改，契约已冻结）。
- 飞行物与 MovementSystem 全量管道联动：本任务用手动欧拉积分推进。
- 网络 RPC / 客户端预测 / 持久化（由上游网关与存档模块承担，§11 / §13）。

## 目录

```
include/mmo/game/combat/skill/   skill_system.h / skill_def.h / skill_registry.h
                                 buff_def.h / cooldown_tracker.h / combat_events.h
src/skill/                       skill_system.cpp / skill_registry.cpp / buff_def.cpp
tests/                           skill_test.cpp
benchmark/                       skill_bench.cpp（输出 bench/skill.txt）
config/gameplay/skills/          offense.json / support.json / utility.json（共 12 条）
config/gameplay/buffs/           buffs.json
docs/                            README / INTERFACE / DEPENDENCY / PERFORMANCE / TEST
```

## State Owner（§4）

技能运行时状态（冷却表、施法进度、飞行物）的 Owner 是 **Combat System**，只在 Scene 线程内写。
Skill 不直接读 Role 私有成员，一切经 `RoleSystem` 接口（`ModifyHp` / `ModifyMp` / `Find`）访问。


---

# TASK-022 · Damage / Heal

统一伤害与治疗结算：**`ComputeDamage`（纯函数）/ `ApplyDamage`（落状态）/ `ApplyHeal`**，
支持物理、法术、真实伤害三系，暴击、闪避、抗性减免、护盾吸收、致死判定与采样日志。

## 范围

- 结算顺序**固定**（§20.2）：暴击 → 闪避 → 抗性 → 护盾 → 扣血 → 致死 → 事件，单测逐条断言。
- 公式参数**全部配置化**：`config/gameplay/combat/formula.json`，代码零硬编码系数。
- 随机来自 **per-Scene 确定性 PRNG**（xorshift128+），种子 = `(SceneId, TickNumber, 序列号)`，
  支持状态 Save/Restore（回放与对账必需，§4 / §19）。
- 护盾先于 HP 扣除，护盾值经 `IShieldSource` 接口注入（真实实现在 TASK-023，本任务只定义接口）。
- HP 的**唯一写入口是 `RoleSystem::ModifyHp`**：DamageSystem 只算不改，遵守单写入者约束（§4）。
- 统计（暴击率 / 闪避率 / 平均伤害 / 总伤害量）+ **采样**日志（每秒 N 条，禁止每条全写）。
- 热路径零堆分配：`alloc_per_damage = 0`（实测值见 PERFORMANCE.md）。

## 目录

```
include/mmo/game/combat/damage/  damage.h / damage_formula.h / damage_system.h / prng.h
src/damage/                      damage_system.cpp / damage_formula.cpp
tests/                           damage_test.cpp
benchmark/                       damage_bench.cpp（输出 bench/damage.txt）
config/gameplay/combat/          formula.json
docs/                            README / INTERFACE / DEPENDENCY / PERFORMANCE / TEST
```

## State Owner（§4）

- **HP 的最终写入权在 Role**（TASK-016 §4）：DamageSystem 经 `RoleSystem::ModifyHp` 改血，
  由 Role 负责钳制到 `[0, MaxHp]` 与 `CharacterDied` 发布。
- DamageSystem 自己持有：PRNG 状态、统计计数器、采样环形缓冲、EntityId→CharacterId 绑定表。
- 战斗侧的 `EntityDied` 事件由 DamageSystem 发布（实体维度），与 Role 的 `CharacterDied`
  （角色维度）**语义不同、互不替代**：前者驱动战斗/AI 反应，后者驱动角色状态与落盘。

## 不在本任务范围

- Buff / 护盾的真实来源（TASK-023）：本任务只定义 `IShieldSource` 接口。
- 仇恨（Threat）系统、PVP 规则、伤害吸收/反伤等衍生机制。
- SkillSystem 与本模块的对接（Skill 侧契约在 TASK-021 已冻结，改接线属独立变更）。
