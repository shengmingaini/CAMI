# QuestSystem · INTERFACE

模块 `server/gamenode/quest`，命名空间 `mmo::game::quest`。TASK-019 事件驱动任务系统。

## 公开接口（冻结契约 §7）

```cpp
namespace mmo::game::quest {

// 四类事件契约（本模块导出，供 AI / Inventory / Scene 发布；下游只可 include 本头）
struct MonsterKilled { uint32_t npc_def_id; PlayerId killer; };       // 16B
struct ItemCollected { uint32_t item_def_id; uint32_t count; PlayerId player; }; // 16B
struct NpcTalked     { uint32_t npc_def_id; PlayerId player; };        // 16B
struct LocationReached { uint32_t zone_id; PlayerId player; };         // 16B
struct QuestCompleted { PlayerId player; QuestId quest; };             // 16B（Quest 发布）

// 玩家只读查询（消费 TASK-016 Role）：等级属 Role 实时状态，Quest 不持有副本
class IPlayerQuery {
  virtual uint32_t LevelOf(PlayerId) const noexcept = 0;
};

class QuestSystem {
  explicit QuestSystem(core::EventBus* bus = nullptr) noexcept;

  // 配置化加载（禁止硬编码任务配置，§21）
  core::Result<size_t> LoadQuests(std::string_view dir);
  core::Result<void>   RegisterDef(QuestDef def);   // 测试注入 / 热更
  const QuestDef*      FindDef(QuestId) const noexcept;
  size_t               DefCount() const noexcept;

  // 生命周期（§7）
  core::Result<void> Accept(PlayerId, QuestId, core::TraceID);
  core::Result<void> Abandon(PlayerId, QuestId, core::TraceID);
  core::Result<void> TurnIn(PlayerId, QuestId, core::TraceID);   // 幂等

  // 事件驱动入口（唯一的进度写入口，§8）
  core::Result<void> OnEvent(const Event&);

  // 观测（§7 / §16）
  const QuestInstance* Find(PlayerId, QuestId) const noexcept;
  size_t ActiveQuests(PlayerId) const noexcept;
  size_t EventHandlerCount() const noexcept;   // 已订阅事件处理器数
  size_t PlayerCount() const noexcept;
  size_t RewardGrantCount() const noexcept;     // 实际发奖次数（幂等验证）
  size_t IndexEntryCount() const noexcept;
  size_t IndexBucketCount() const noexcept;
  size_t EventCount() const noexcept;

  // 依赖注入（非拥有引用）
  core::Result<void> BindEventBus(core::EventBus&);  // 幂等，重复调用不翻倍
  void BindPlayerQuery(IPlayerQuery&) noexcept;
  void BindRewardSink(IRewardSink&) noexcept;
};
}
```

## 进度推进模型（§8）

```
MonsterKilled/ItemCollected/NpcTalked/LocationReached  ──(EventBus)──▶  OnEvent(Event)
        │                                                              │
        │ 倒排索引 Lookup(type, target_id, player) 三键命中 O(1)       │
        ▼                                                              ▼
   只更新命中条目的进度；达标 → 退出索引 + RefreshCompletion → 全部达标 → Completed + 发布 QuestCompleted
   零命中 → O(1) 直接返回，不触碰任何玩家数据
```

`OnEvent` 是**唯一的进度写入口**；订阅总线只是把上游强类型事件翻译成统一 `Event` 信封后转调
`OnEvent`。手动调用 `OnEvent` 与经总线派发语义完全一致。

## 配置加载

`LoadQuests(dir)` 从目录读取 `*.json`（受限 JSON 解析器，无第三方依赖）。缺字段 / 引用非法
（缺 `id`、未知目标类型、目标数量 0、目录不存在）一律返回明确错误码，**禁止默认值静默生成**。
目标数量字段名为 `count`，映射到 `ObjectiveDef::required_count`。
