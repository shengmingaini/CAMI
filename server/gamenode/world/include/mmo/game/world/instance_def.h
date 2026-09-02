#pragma once

/// TASK-020 · World / Instance 公共类型（§7 / §15.1）。
///
/// 命名空间：mmo::game::world。实例五状态机（Pending/Loading/Running/Completed/Destroying）
/// 与 Scene 五状态机（Creating/Loading/Running/Draining/Destroying）相互独立：本文件的状态
/// 是「副本实例」的生命周期，Scene 是「场景」的实时仿真状态（§8）。
///
/// 类型归属提醒（§27.2 / TASK-012 红线）：SceneId / PlayerId / NodeId / LeaveReason 定义在
/// `mmo::game`（非 `mmo::game::scene`），本文件直接使用，禁止误写为 `scene::SceneId`。

#include <cstdint>
#include <vector>

#include "mmo/core/time/clock.h"            // SteadyTime / DurationMs
#include "mmo/game/entity/entity_id.h"      // SceneId
#include "mmo/game/scene/scene_id.h"        // PlayerId / NodeId / LeaveReason
#include "mmo/game/ai/spawn_def.h"          // SpawnDef

namespace mmo::game::world {

/// 实例五状态机（§8）：Pending → Loading → Running → Completed → Destroying。
/// 任一中间态失败均可跳到 Destroying（§8 失败路径）。
enum class InstanceState : std::uint8_t {
    Pending,     // 已分配 ID，尚未创建 Scene
    Loading,     // 创建 Scene + 生成怪物/NPC 中
    Running,     // 玩家可进入，副本运营中
    Completed,   // 结算完成（超时/通关），等待宽限后回收
    Destroying,  // 回收中（宽限到期 / 立即），下一 Tick 真正析构
};

/// 实例类型（与 SceneType 数值对齐，便于 SceneId 编码，§15.1）。
enum class InstanceType : std::uint8_t {
    OpenWorld = 0,
    Dungeon = 1,
    Arena = 2,
    Battleground = 3,
    TemporaryInstance = 4,
};

/// 实例结算结果（Complete 入参，§7）。
enum class InstanceResult : std::uint8_t {
    Cleared,     // 通关
    Failed,      // 失败
    Abandoned,   // 放弃
    Timeout,     // 超时强制结算
};

/// 实例全局唯一 ID（单调递增，永不复用，§15.2）。
using InstanceId = std::uint64_t;

/// 分线 / 回收策略（§8 / §15.4）。WorldManager::Init 注入，实例层据此回收。
struct WorldConfig {
    std::uint32_t   sharding_threshold = 300;             // OpenWorld 单 Scene 玩家超此值开分线
    core::DurationMs empty_instance_timeout{300'000};      // 创建后无人进入 > 5min 回收（空实例）
    core::DurationMs destroy_grace{60'000};               // 全员退出 / 结算后延迟 60s 销毁（防误杀）
};

/// 实例静态定义：全部来自配置，代码零硬编码（§15.1 / §21 Forbidden）。
/// name / scene_asset 为 string_view，指向加载器持有的字符串池（生命周期由 WorldConfigBundle 保证）。
struct InstanceDef {
    std::uint32_t def_id{0};
    InstanceType  type{InstanceType::Dungeon};
    std::string_view name;                  // 展示名（元数据）
    std::uint32_t max_players{0};           // 成员上限（AddMember 校验）
    std::uint32_t min_players{0};           // 最小人数（组队校验用，本任务仅存不校验）
    core::DurationMs time_limit{0};         // 0 = 无限；>0 到期转 Completed
    std::vector<mmo::game::ai::SpawnDef> spawn_defs;  // 副本内怪物/NPC 生成定义
    std::string_view scene_asset;           // 静态场景资源引用（配置化）
};

/// 实例运行时快照（§7，冻结契约对外字段）。
/// 额外内部簿记字段（destroy_at / pending_reclaim / ever_entered / last_result）不破坏
/// 对外字段语义，仅用于回收决策（§15.4 / §19）。
struct Instance {
    InstanceId      id{0};
    std::uint32_t   def_id{0};
    mmo::game::SceneId scene_id{0};
    InstanceState   state{InstanceState::Pending};
    std::vector<mmo::game::PlayerId> members;
    core::SteadyTime created_at{};
    core::SteadyTime started_at{};
    core::DurationMs  elapsed{0};
    std::uint32_t   version{0};

    // ---- 内部回收簿记（非对外契约字段，仅供 InstanceManager 回收决策） ----
    core::SteadyTime destroy_at{};     // 计划转入真正析构的时刻（宽限到期）
    bool            pending_reclaim{false};  // 已决定回收，等待宽限
    bool            ever_entered{false};     // 是否曾有玩家加入（区分空实例）
    InstanceResult  last_result{InstanceResult::Cleared};  // 最近一次结算结果
};

}  // namespace mmo::game::world
