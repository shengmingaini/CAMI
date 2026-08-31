#pragma once

/// TASK-012 · Scene 结构、五状态机、Enter/Leave/Tick/StateHash（§7 / §15.2 / §15.5 / §15.7 / §15.8）。
///
/// 依赖：TASK-011 EntityManager（创建 / 延迟销毁 Avatar 实体，§15.5 / §21）。
/// Scene 是实时状态的权威 Owner（§4）：只有持有该 Scene 的 GameNode Simulation 线程可写。
///
/// 模块边界（§27.3）：本头只暴露公开接口；下游只能通过 include/ 调用，禁止 include src/。

#include <cstdint>
#include <unordered_map>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/game/entity/entity_id.h"        // EntityId / SceneId
#include "mmo/game/entity/entity_manager.h"   // EntityManager（§27.2 消费公开接口）
#include "mmo/game/scene/scene_id.h"          // PlayerId / NodeId / LeaveReason

namespace mmo::game {

/// Scene 五状态机（§8）：Creating → Loading → Running → Draining → Destroying。
/// 任一中间态都可失败跳到 Destroying（§8 失败路径）。
enum class SceneState : std::uint8_t {
    Creating,
    Loading,
    Running,
    Draining,
    Destroying,
};

/// Scene 类型（§7）。
enum class SceneType : std::uint8_t {
    World = 0,
    Dungeon = 1,
    Arena = 2,
    Battleground = 3,
    TemporaryInstance = 4,
};

// SceneContext 在 scene_context.h 定义；Tick 仅按 const 引用使用，此处前向声明即可，
// 避免 scene.h ↔ scene_context.h 循环包含（scene_context.h 需 SceneType，故包含 scene.h）。
struct SceneContext;

/// 单个场景：实时仿真基本单位（§1）。
class Scene {
public:
    Scene(SceneId id, SceneType type, NodeId owner,
          EntityManager& entities, core::EventBus& events);

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // ---- 标识 / 状态（§7；Acceptance #2 五项可 grep 结构体）----
    SceneId Id() const noexcept;             // SceneID
    SceneType Type() const noexcept;
    SceneState State() const noexcept;
    std::uint32_t Version() const noexcept;        // SceneVersion
    std::uint64_t TickNumber() const noexcept;     // TickNumber
    std::uint64_t StateHash() const noexcept;      // StateHash
    NodeId OwnerNode() const noexcept;             // OwnerGameNode

    // ---- 玩家 / 实体（§7 / §15.5 / §15.7 / §15.8）----
    /// 绑定玩家与其 Avatar 实体（Avatar 由调用方经 EntityManager 创建后传入，§7 签名）。
    /// 仅 Running 态可进入；超容量返回 BUSY；重复进入返回 INVALID_ARGUMENT。
    core::Result<void> Enter(PlayerId, EntityId avatar);
    /// 解绑玩家并延迟销毁其 Avatar（EntityManager::Destroy 仅逻辑死亡，物理回收在 FlushDeferred）。
    core::Result<void> Leave(PlayerId, LeaveReason);
    /// 由 SceneManager::TickAll 驱动（§7：本任务只留接口，调度由 TASK-013 负责）。
    core::Result<void> Tick(const SceneContext&);
    std::size_t PlayerCount() const noexcept;
    std::size_t EntityCount() const noexcept;

    // ---- 生命周期（§7 / §8）----
    /// 状态转移；非法转移返回 INVALID_ARGUMENT（§16）。
    core::Result<void> TransitionTo(SceneState);
    /// 增量滚动哈希，每 N Tick（默认 60）由 Tick 调用一次（§15.6 / §22），确定性可复现。
    core::Result<void> ComputeStateHash();

private:
    // ---- 五项核心状态（Acceptance #2 grep：SceneID/SceneVersion/TickNumber/StateHash/OwnerGameNode）----
    SceneId id_;                // SceneID
    std::uint32_t version_{1};  // SceneVersion
    std::uint64_t tick_number_{0};  // TickNumber
    std::uint64_t state_hash_{0};   // StateHash
    NodeId owner_node_;        // OwnerGameNode

    SceneType type_;
    SceneState state_;

    // 共享系统引用（§27.2 消费上游公开接口；热路径禁止拥有线程）
    EntityManager& entities_;
    core::EventBus& events_;

    // 玩家 → Avatar 映射（有界，≤ max_players_，§15.8 / §21 禁止无界）
    std::unordered_map<PlayerId, EntityId> players_;
    std::size_t entity_count_{0};  // 已绑定 Avatar 数（§15.7 实体数上限）
    std::size_t max_players_;
    std::size_t max_entities_;
    std::uint64_t state_hash_period_;

    static constexpr std::size_t kDefaultMaxPlayers = 2000;
    static constexpr std::size_t kDefaultMaxEntities = 100'000;
    static constexpr std::uint64_t kDefaultHashPeriod = 60;

    bool IsLegalTransition(SceneState from, SceneState to) const noexcept;
    void PublishStateEvent(SceneState to);
};

}  // namespace mmo::game
