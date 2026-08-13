// game/ecs/components.h — ECS 组件定义 (阶段 A W3)
//
// 设计: 全部组件为 POD / trivially-copyable, 零虚函数、零指针,
// 天然适配 Structure-of-Arrays 连续存储 (SIMD 友好, cache 友好)。
// 热路径零堆分配: 实体数据按组件类型连续排布, 迭代无随机访存。
//
// 本文件不依赖 Entt, OFF 构建即可编译并单测 (验证 SoA 内存布局)。
// Entt 仅作为 world/视图调度器 (见 ecs_world.h, 门控 CAMI_BUILD_MODULES)。
#pragma once

#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace cami::ecs {

// ---- 几何/变换 ----
struct Transform {
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;   // 弧度
};

// ---- 运动 ----
struct Velocity {
    float vx = 0.f, vy = 0.f, vz = 0.f;
    float speed = 0.f;
};

// ---- 生存属性 ----
struct Health {
    std::int32_t hp = 1;
    std::int32_t maxHp = 1;
};

struct Mana {
    std::int32_t mana = 0;
    std::int32_t maxMana = 0;
};

// ---- 成长 ----
struct Level {
    std::uint16_t level = 1;
    std::uint32_t exp = 0;
};

// ---- 战斗状态 (不存角色数据, 仅战斗模块运行态) ----
struct CombatState {
    std::uint64_t targetId = 0;   // 0 = 无目标
    std::uint8_t  moveState = 0;  // 见 capnp MoveState 枚举值
    bool          inCombat = false;
};

// ---- 阵营/声望 ----
struct Faction {
    std::uint16_t factionId = 0;
    std::int32_t  reputation = 0;
};

// ---- 网络身份 (仅玩家实体; 怪物/NPC 无) ----
struct NetworkId {
    std::uint64_t playerId = 0;
    std::uint32_t connectionId = 0;
};

// ---- AI 运行态 (怪物/NPC) ----
struct AiState {
    std::uint8_t  behavior = 0;   // 0=idle,1=patrol,2=aggro,3=leash
    std::uint64_t homeCellId = 0;
};

// 编译期断言: 所有组件必须 trivially-copyable (SoA 连续搬运安全)。
static_assert(std::is_trivially_copyable_v<Transform>,  "Transform must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Velocity>,   "Velocity must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Health>,     "Health must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Mana>,       "Mana must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Level>,      "Level must be trivially copyable");
static_assert(std::is_trivially_copyable_v<CombatState>,"CombatState must be trivially copyable");
static_assert(std::is_trivially_copyable_v<Faction>,    "Faction must be trivially copyable");
static_assert(std::is_trivially_copyable_v<NetworkId>,  "NetworkId must be trivially copyable");
static_assert(std::is_trivially_copyable_v<AiState>,    "AiState must be trivially copyable");

}  // namespace cami::ecs
