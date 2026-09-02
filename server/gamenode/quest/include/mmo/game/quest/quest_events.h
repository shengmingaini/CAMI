#pragma once

/// TASK-019 · Quest 事件契约（§8 数据模型）。
///
/// 上游未提供这四类事件（TASK-018 未导出 MonsterKilled，TASK-017 的 ItemAdded 属
/// inventory 私有契约），故由本模块在 include/ 下定义并导出，作为 Quest 的公开事件契约：
/// 生产者（AI / Inventory / Scene）发布本头中的事件，Quest 消费；下游只能 include 本头。
///
/// 全部 ≤ 32B，满足 EventBus 内联预算（禁止堆分配，§9 热路径）。

#include <cstdint>

#include "mmo/game/quest/quest_def.h"

namespace mmo::game::quest {

/// 击杀怪物（由 AI / Combat 在怪物死亡时发布）。16B
struct MonsterKilled {
    std::uint32_t npc_def_id{0};
    PlayerId killer{0};
};

/// 拾取 / 获得物品（由 Inventory 发布）。16B
struct ItemCollected {
    std::uint32_t item_def_id{0};
    std::uint32_t count{1};
    PlayerId player{0};
};

/// 与 NPC 对话（由 Scene / Quest 触发点发布）。16B
struct NpcTalked {
    std::uint32_t npc_def_id{0};
    PlayerId player{0};
};

/// 到达区域（由 Movement / Scene 触发点发布）。16B
struct LocationReached {
    std::uint32_t zone_id{0};
    PlayerId player{0};
};

/// 任务完成（Quest 发布，供 Achievement / Social 等下游消费）。16B
struct QuestCompleted {
    PlayerId player{0};
    QuestId quest{0};
};

static_assert(sizeof(MonsterKilled) <= 32, "事件 ≤ 32B（EventBus 内联预算）");
static_assert(sizeof(ItemCollected) <= 32, "事件 ≤ 32B（EventBus 内联预算）");
static_assert(sizeof(NpcTalked) <= 32, "事件 ≤ 32B（EventBus 内联预算）");
static_assert(sizeof(LocationReached) <= 32, "事件 ≤ 32B（EventBus 内联预算）");
static_assert(sizeof(QuestCompleted) <= 32, "事件 ≤ 32B（EventBus 内联预算）");

}  // namespace mmo::game::quest
