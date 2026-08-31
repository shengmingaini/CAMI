#pragma once

/// TASK-016 · RoleSystem 公开接口（§7，冻结契约）。
///
/// State Owner（§4）：角色数据由所属 Scene 的 SimulationThread 独占写入（单 Owner，§9）。
/// Role **不负责** Scene / AOI / 网络 / MySQL（§4 / §21）：
///   · 场景实体绑定只记录 avatar / scene 两个 id，不反向持有 Scene；
///   · 落盘只经 IPersistenceAdapter 异步投递，Tick 内绝不等待（§11 / §13）。
///
/// 注意：SceneContext 定义在 namespace mmo::game（TASK-012），
/// **不存在 mmo::game::scene 命名空间**，故本文件用无限定名 SceneContext。

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace mmo::game::role {

/// 角色初始值与成长（构造期注入，避免散落在逻辑里；§21 的硬编码禁令针对经验曲线）。
struct RoleDefaults {
    std::int64_t starting_primary{10};   // 1 级时 Str/Agi/Int/Sta
    std::int64_t primary_per_level{2};   // 每级主属性成长
};

/// 运行期统计（§6 观测性）。
struct RoleStats {
    std::size_t save_enqueued{0};   // 成功入队次数
    std::size_t save_failed{0};     // 入队失败次数（已转入重试队列）
    std::size_t level_ups{0};       // 累计升级次数
    std::size_t deaths{0};          // 累计死亡事件数
};

class RoleSystem {
public:
    /// sink 为必填：Role 禁止直连存储（§21）。curve 由配置加载，禁止默认值静默启动（§19）。
    RoleSystem(IPersistenceAdapter& sink, ExpCurve curve, RoleDefaults def = {}) noexcept;

    /// 绑定事件总线。§7 冻结签名里 ModifyHp/ModifyMp/AddExp **不接收 SceneContext**，
    /// 因此事件发布依赖构造后绑定的总线（Role 由 Scene 拥有，Scene 初始化时绑定一次）。
    /// LoadOrCreate 额外接收 ctx，会顺带把总线缓存下来。
    void BindEventBus(core::EventBus& bus) noexcept { events_ = &bus; }

    /// 加载角色；不存在则按 RoleDefaults 创建 1 级新角色（幂等）。
    core::Result<Character*> LoadOrCreate(PlayerId, CharacterId, const SceneContext&);

    /// 绑定场景实体（只记 id，不持有 Scene 对象）。
    core::Result<void> AttachToScene(CharacterId, EntityId avatar, SceneId);

    /// 改 HP：钳制到 [0, MaxHp]；归零置死亡标记并发布 CharacterDied（只发一次，§15.7）。
    /// 战斗语义（伤害公式/闪避）在 TASK-022，这里只改数据（§7 注释）。
    core::Result<void> ModifyHp(CharacterId, std::int64_t delta, core::TraceID);

    /// 改 MP：钳制到 [0, MaxMp]，无死亡语义。
    core::Result<void> ModifyMp(CharacterId, std::int64_t delta, core::TraceID);

    /// 加经验：可跨多级连续升级，返回**新等级**。溢出 → Fail（§19）。
    core::Result<std::uint32_t> AddExp(CharacterId, std::uint64_t amount, core::TraceID);

    /// 三层来源变更后重算派生属性，并把 HP/MP 重新钳制到新上限。
    core::Result<void> RecomputeAttributes(CharacterId);

    /// 异步存档：只入队，不等待落盘（§11）。失败转入重试队列并返回 Fail（§19）。
    core::Result<void> Save(CharacterId);

    /// 重投重试队列（§19 可恢复）：由 Persistence/定时线程在后端恢复后调用，
    /// 成功的移除出队列。返回本次成功重投条数；Tick 内不等待任何 IO。
    std::size_t FlushRetries() noexcept;

    /// 预留容器容量（§10 热路径零分配）：批量载入前调用，避免中途 rehash。
    void Reserve(std::size_t n);

    // ---- 只读访问（非 owning，生命周期由本系统持有）----
    Character* Find(CharacterId) noexcept;
    Character* FindByPlayer(PlayerId) noexcept;

    std::size_t CharacterCount() const noexcept { return chars_.size(); }
    /// 存档失败待重试的角色数（§19）。
    std::size_t RetryQueueSize() const noexcept { return retry_queue_.size(); }
    const std::vector<CharacterId>& RetryQueue() const noexcept { return retry_queue_; }

    RoleStats Stats() const noexcept { return stats_; }
    void ResetStats() noexcept { stats_ = RoleStats{}; }

    const ExpCurve& Curve() const noexcept { return curve_; }
    const RoleDefaults& Defaults() const noexcept { return defaults_; }

private:
    Character* FindOrNull(CharacterId) noexcept;
    /// 升级后处理：主属性成长 → Recompute → HP/MP 重新钳制。
    void ApplyLevelUp(Character& c) noexcept;
    void ClampVitals(Character& c) noexcept;
    /// 事件发布（未绑定总线时静默跳过，保证纯数据场景可用）。
    template <typename T>
    void Emit(const T& ev) noexcept {
        if (events_ != nullptr) (void)events_->Publish(ev);
    }

    core::EventBus* events_{nullptr};

    IPersistenceAdapter& sink_;
    ExpCurve curve_;
    RoleDefaults defaults_;
    std::unordered_map<CharacterId, Character> chars_;
    std::unordered_map<PlayerId, CharacterId> by_player_;
    std::vector<CharacterId> retry_queue_;
    RoleStats stats_;
};

}  // namespace mmo::game::role
