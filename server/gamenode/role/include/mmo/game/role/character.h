#pragma once

/// TASK-016 · Character 数据（§7 / §8）。
///
/// State Owner（§4）：角色数据由所属 Scene 的 SimulationThread 独占写入（单 Owner）。
/// **实时 HP/MP 的权威在 Scene / Combat（TASK-022）**——本结构里的 hp/mp 是 Role 侧数据镜像，
/// Role 只负责按 MaxHp/MaxMp 做钳制与事件发布，禁止把这里的 hp 当作跨模块权威。
/// 持久化副本归 DataService，Role 只能通过 IPersistenceAdapter 异步投递（§11 / §13 / §21）。

#include <cstdint>
#include <string>

#include "mmo/game/entity/entity_id.h"      // EntityId（TASK-011）
#include "mmo/game/role/attribute.h"          // AttributeSet
#include "mmo/game/scene/scene_id.h"          // SceneId / PlayerId（TASK-012）

namespace mmo::game::role {

/// 角色唯一标识（本模块自有类型；PlayerId 复用 TASK-012 定义，禁止第二套）。
using CharacterId = std::uint64_t;

using PlayerId = mmo::game::PlayerId;
using EntityId = mmo::game::EntityId;
using SceneId = mmo::game::SceneId;

inline constexpr CharacterId kInvalidCharacterId = 0;

/// 角色状态标志位（§15.7 死亡状态）。
inline constexpr std::uint32_t kCharFlagDead = 1u << 0;   // 死亡（HP=0）
inline constexpr std::uint32_t kCharFlagDirty = 1u << 1;  // 有未落盘的变更

inline bool IsDead(std::uint32_t flags) noexcept {
    return (flags & kCharFlagDead) != 0u;
}

/// 角色（§7）。sizeof 见 role_bench 实测（§22 预算 < 512B）。
struct Character {
    CharacterId id{kInvalidCharacterId};
    PlayerId owner{0};
    std::string name;                 // 角色名（32B，SSO 不额外分配短名）
    std::uint32_t level{1};
    std::uint64_t exp{0};             // 当前等级内累计经验
    std::int64_t hp{0};
    std::int64_t mp{0};
    AttributeSet attrs;               // 三层属性 + 派生 Final（352B）
    std::uint32_t version{0};         // 每次数据变更 +1（乐观校验 / 存档去抖）
    std::uint32_t flags{0};           // kCharFlag*
    EntityId avatar{0};               // AttachToScene 绑定的场景实体
    SceneId scene{0};                 // 所属场景

    std::int64_t MaxHp() const noexcept { return attrs.Total(AttrType::MaxHp); }
    std::int64_t MaxMp() const noexcept { return attrs.Total(AttrType::MaxMp); }
};

}  // namespace mmo::game::role
