#pragma once

/// TASK-019 · Quest 定义（§7 / §15.1）—— 全部配置化，禁止硬编码（§21）。
///
/// 所有任务定义来自 config/gameplay/quests/*.json，由 src/quest_config.cpp 加载。
/// 类型别名与上游保持同一底层类型（不引入第二套实体/玩家类型，§27.3）：
///   PlayerId : uint64（同 TASK-012 scene / TASK-016 role）
///   ItemId   : uint64（同 TASK-017 inventory）

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mmo::game::quest {

using QuestId = std::uint32_t;

/// 玩家标识：与 TASK-012 scene 的 `mmo::game::PlayerId`、TASK-016 role 同型（uint64）。
using PlayerId = std::uint64_t;

/// 物品定义标识：与 TASK-017 inventory 的 `mmo::game::ItemId` 同型（uint64）。
using ItemId = std::uint64_t;

/// 目标类型（§7）。倒排索引以此为一级键。
enum class ObjectiveType : std::uint8_t {
    KillMonster = 0,
    CollectItem,
    TalkNpc,
    ReachLocation,
    UseItem,
};

constexpr std::size_t kObjectiveTypeCount = 5;

/// 目标类型名（日志 / 指标标签 / 配置解析用）。
const char* ToString(ObjectiveType t) noexcept;

/// 单个目标定义（§7）。
struct ObjectiveDef {
    ObjectiveType type{ObjectiveType::KillMonster};
    std::uint32_t target_id{0};        // 怪物 / 物品 / NPC / 区域 / 道具 定义 id
    std::uint32_t required_count{1};   // 需要的数量（TalkNpc/ReachLocation 为一次性）
};

/// 任务定义（§7）。注：任务书写作 `std::string_view title`，但定义由配置加载并长期
/// 持有，string_view 会悬垂 —— 故改为 `std::string`（持有副本），语义不变。
struct QuestDef {
    QuestId id{0};
    std::string title;
    std::uint32_t required_level{1};
    std::vector<QuestId> prerequisites;      // 前置任务（全部 TurnedIn 后才可选）
    std::vector<ObjectiveDef> objectives;
    std::uint64_t exp_reward{0};
    std::uint64_t currency_reward{0};
    std::vector<ItemId> item_rewards;
};

}  // namespace mmo::game::quest
