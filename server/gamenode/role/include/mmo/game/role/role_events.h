#pragma once

/// TASK-016 · 角色事件（§15.5 / §15.7）。
///
/// 约束：EventBus 内联发布要求事件值类型 ≤ 32B、nothrow move、alignof ≤ 8。
/// 全部事件为 POD，无指针、无所有权，跨模块按值消费。
/// 注：HP 变更事件在 TASK-022 被伤害系统复用（§15.5）。

#include <cstdint>

#include "mmo/game/role/character.h"

namespace mmo::game::role {

/// 属性重算完成（RecomputeAttributes / 升级 / 装备或 Buff 变更后）。16B
struct AttributesChanged {
    CharacterId id{0};
    std::uint32_t version{0};
    std::uint32_t new_level{0};
};

/// 升级（每升一级发一次，跨多级时连续发布）。16B
struct LevelUp {
    CharacterId id{0};
    std::uint32_t new_level{0};
    std::uint32_t version{0};
};

/// HP 变更（delta 为实际生效值，非请求值——钳制后的结果）。24B
struct HpChanged {
    CharacterId id{0};
    std::int64_t hp{0};       // 变更后 HP（已钳制到 [0, MaxHp]）
    std::int64_t delta{0};    // 实际生效增量
};

/// MP 变更（语义同 HpChanged）。24B
struct MpChanged {
    CharacterId id{0};
    std::int64_t mp{0};
    std::int64_t delta{0};
};

/// 死亡（HP 归零瞬间发布一次，重复 ModifyHp 不重复发布）。16B
struct CharacterDied {
    CharacterId id{0};
    SceneId scene{0};
};

static_assert(sizeof(AttributesChanged) <= 32);
static_assert(sizeof(LevelUp) <= 32);
static_assert(sizeof(HpChanged) <= 32);
static_assert(sizeof(MpChanged) <= 32);
static_assert(sizeof(CharacterDied) <= 32);

}  // namespace mmo::game::role
