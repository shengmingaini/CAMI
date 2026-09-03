# SkillSystem · README

MMORPG 技能系统（TASK-021，Phase 4 基础 MMORPG）。

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
- 伤害管线（暴击 / 护甲 / 仇恨）：本任务只结算 `Damage` / `Heal` 基础公式并发布事件。
- 飞行物与 MovementSystem 全量管道联动（TASK-022 统一）：本任务用手动欧拉积分推进。
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
