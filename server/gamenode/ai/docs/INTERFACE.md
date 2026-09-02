# AiSystem · INTERFACE

模块 `server/gamenode/ai`，命名空间 `mmo::game::ai`。TASK-018 NPC / Monster / AI 第一版状态机。

## 公开接口（冻结契约 §7）

```cpp
enum class AiState : uint8_t { Idle, Patrol, Chase, Attack, Return, Dead };
constexpr const char* ToString(AiState) noexcept;
struct SpawnDef { ... };                 // 全部字段来自配置，无硬编码
struct AiComponent { ... };              // 单 AI 黑板，≈56B
class AiSystem {
  AiSystem(EntityManager&, movement::MovementSystem&, aoi::IAoi&, core::Scheduler&) noexcept;
  core::Result<mmo::game::EntityId> Spawn(const SpawnDef&, const scene::SceneContext&);
  core::Result<void>          Despawn(mmo::game::EntityId);
  core::Result<void>          Update(const scene::SceneContext&);   // AI 阶段驱动
  core::Result<void>          OnDamaged(mmo::game::EntityId victim, mmo::game::EntityId attacker, int64_t amount);
  AiState StateOf(mmo::game::EntityId) const noexcept;
  size_t  CountByState(AiState) const noexcept;
};
```

## 状态机（§8，表驱动）

```
Idle ─(巡逻)─> Patrol ─(发现目标)─> Chase ─(进入攻击距离)─> Attack
  ▲              │                    │(脱离追击半径)         │(目标死亡/消失)
  │              └────────────────────┘                      ▼
  └──────────────── Return ──(回到原点)───────┘<──────────────┘
Dead <──(HP<=0)── 任意状态；Dead ──(Scheduler 重生)──> Idle
```

任意转移必须先过 `CanTransition(from, to)`（见 `ai_state.cpp` 的 `kAllowed` 矩阵），
决策函数只算「期望下一态」，未授权则保持原态。

## 配置加载

`LoadSpawnDefs(path)` / `LoadSpawnDefsFromDir(dir)` 从 `config/gameplay/npc/*.json`
读取生成定义，自带最小 JSON 解析器，失败返回错误（禁止默认值静默生成）。
