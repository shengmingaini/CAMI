// game/ecs/ecs_world.h — Entt 封装的 ECS 世界 (阶段 A W3)
//
// 设计依据: 白皮书 v4.0 §2.2 (Cell 分片 + ECS SoA + 共享内存 IPC)。
// Entt 为 header-only MIT 库, 提供 registry / view / group 调度。
// 组件本身是纯 POD (components.h), 与调度器解耦 —— 替换 Entt 不影响组件布局。
//
// 门控: 仅在 CAMI_BUILD_MODULES=ON 时可见 (Entt 由 vcpkg 提供, 本地未 bootstrap)。
// OFF 构建不引用本文件, 零额外依赖。
#pragma once

#ifdef CAMI_BUILD_MODULES

#include <entt/entt.hpp>
#include <cstdint>
#include "game/ecs/components.h"

namespace cami::ecs {

class EcsWorld {
public:
    // 创建一个空实体 (无组件)
    entt::entity spawn() { return reg_.create(); }

    // 创建一个玩家实体并附核心组件 (Transform/Health/Mana/Level/NetworkId/CombatState)
    entt::entity spawnPlayer(std::uint64_t playerId, std::uint32_t connectionId,
                             float x = 0.f, float y = 0.f, float z = 0.f, float yaw = 0.f,
                             std::int32_t maxHp = 100, std::int32_t maxMana = 100) {
        entt::entity e = reg_.create();
        // 组件为聚合类型 (无用户构造): C++17 下 Entt emplace 用括号直接初始化
        // 无法初始化聚合, 故先构造临时对象再 emplace (拷贝/移动, 保持 trivially-copyable)。
        reg_.emplace<Transform>(e, Transform{x, y, z, yaw});
        reg_.emplace<Health>(e, Health{maxHp, maxHp});
        reg_.emplace<Mana>(e, Mana{maxMana, maxMana});
        reg_.emplace<Level>(e, Level{1u, 0u});
        reg_.emplace<CombatState>(e, CombatState{0u, 0u, false});
        reg_.emplace<NetworkId>(e, NetworkId{playerId, connectionId});
        return e;
    }

    // 创建怪物/NPC 实体
    entt::entity spawnNpc(std::uint64_t homeCellId, std::uint8_t behavior = 1,
                          float x = 0.f, float y = 0.f, float z = 0.f,
                          std::int32_t maxHp = 50) {
        entt::entity e = reg_.create();
        reg_.emplace<Transform>(e, Transform{x, y, z, 0.f});
        reg_.emplace<Health>(e, Health{maxHp, maxHp});
        reg_.emplace<AiState>(e, AiState{behavior, homeCellId});
        return e;
    }

    // 附加/覆盖组件
    template <typename C, typename... Args>
    C& attach(entt::entity e, Args&&... args) {
        return reg_.emplace<C>(e, std::forward<Args>(args)...);
    }

    // 读取组件 (调用方需保证组件已存在)
    template <typename C>
    C& get(entt::entity e) {
        return reg_.get<C>(e);
    }

    bool alive(entt::entity e) const { return reg_.valid(e); }

    void destroy(entt::entity e) { reg_.destroy(e); }

    // SoA 友好迭代: view 按组件类型批量遍历 (cache 友好, 无随机访存)
    template <typename... C, typename F>
    void each(F&& fn) {
        reg_.view<C...>().each(std::forward<F>(fn));
    }

    std::size_t alive_count() const { return reg_.alive(); }

    // 直接访问底层 registry (高级用法: group / storage / 信号)
    entt::registry& raw() { return reg_; }

private:
    entt::registry reg_;
};

}  // namespace cami::ecs

#endif  // CAMI_BUILD_MODULES
