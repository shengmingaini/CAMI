#pragma once

/// TASK-018 · SpawnDef —— 生成定义，全部字段来自配置（§7 / §15.2 / §21 Forbidden）。
///
/// 禁止在 AI 逻辑里硬编码任何 NPC / Monster 数值；所有半径、血量、重生时间均由调用方
/// 从 config/gameplay/npc/*.json 取出后填入本结构（见 npc_config.h 的加载器）。

#include <cstdint>
#include <string_view>

#include "mmo/game/entity/entity.h"   // mmo::game::EntityType / mmo::game::Position / mmo::game::EntityId
#include "mmo/game/ai/ai_state.h"     // kInvalidEntity

namespace mmo::game::ai {

/// 生成定义：一个 NPC / Monster 类型的全部可调参数。
struct SpawnDef {
    std::uint32_t      npc_def_id{0};                       // 配置 ID（与 json 对应）
    mmo::game::EntityType type{mmo::game::EntityType::Monster};   // Monster / Npc
    std::string_view   name;                                // 展示名（元数据，可为空）
    mmo::game::Position   spawn_pos{};                         // 出生点 == 重生点
    float              patrol_radius{10.0f};                // 巡逻半径（米）
    float              aggro_radius{15.0f};                 // 仇恨触发半径（米）
    float              chase_leave_radius{30.0f};           // 脱离追击半径（米）
    std::uint32_t      respawn_seconds{30};                 // 死亡后重生延迟（秒）
    std::int64_t       max_hp{100};                         // 血量上限（AI 内部镜像，权威归 Combat）
    std::uint32_t      level{1};                            // 等级
};

}  // namespace mmo::game::ai
