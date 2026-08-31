#pragma once

/// TASK-015 · Movement System 公开接口（§7，冻结契约）。
///
/// 命名空间：mmo::game::movement。
/// 状态归属（§4）：玩家坐标与朝向的唯一权威写入者，只在 Movement 阶段写入；
/// 防加速 / 防穿墙校验在写入点完成。客户端上报坐标是意图，不是权威。
///
/// 跨模块边界（§27.2）：消费 TASK-011 EntityManager、TASK-012 SceneContext、
/// TASK-014 aoi::IAoi 的**公开接口**，禁止 include 其 src/。

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/entity/entity_id.h"         // EntityId
#include "mmo/game/movement/movement_state.h"   // MovementState / MoveCommand / MoveReject / MovementStats / MovementConfig
#include "mmo/game/aoi/aoi.h"                  // aoi::IAoi（§27.2 消费公开接口）
#include "mmo/game/scene/scene_context.h"      // SceneContext（§27.2）

namespace mmo::game::movement {

/// 移动系统（§7）。
class MovementSystem {
public:
    /// cfg 为系统配置；aoi 为可选 AOI 联动实例（不绑定时跳过 AOI 更新，仅更新权威位置）。
    explicit MovementSystem(MovementConfig cfg = {}, aoi::IAoi* aoi = nullptr) noexcept
        : cfg_(cfg), aoi_(aoi) {}

    /// 绑定 / 解绑 AOI 实例（§15.6 联动；运行期可切换，单 Owner 模拟线程安全）。
    void BindAoi(aoi::IAoi& aoi) noexcept { aoi_ = &aoi; }
    void UnbindAoi() noexcept { aoi_ = nullptr; }

    // ---- 命令 / 校验 ----

    /// 应用一条客户端移动命令：校验 → 修正/拒绝 → 更新权威状态 → 写实体 → 触发 AOI Move。
    /// 拒绝（Teleport/NotMovable/RateLimited/非有限坐标）保持原位置并计数；返回 Fail。
    /// 软接受（TooFast/OutOfBounds）钳制到合法位置后接受并计数。
    core::Result<void> ApplyCommand(const MoveCommand& cmd, const SceneContext& ctx);

    /// Tick 内积分：按各实体速度推进位置（pos += velocity*dt），钳制世界边界，写实体，触发 AOI。
    /// 无速度实体跳过；超过 stop_timeout 无命令上报者外推停止（velocity 清零）。
    core::Result<void> Integrate(const SceneContext& ctx, float dt_seconds);

    /// 纯函数校验（委托 validator.h 的 ValidateMovement），便于单测逐条触发规则。
    core::Result<MoveReject> Validate(const MoveCommand& cmd,
                                      const MovementState& state) const noexcept;

    // ---- 速度 / 停止 ----

    /// 设置实体最大速度（由服务端属性决定，禁止采用客户端速度字段，§21）。
    core::Result<void> SetSpeed(EntityId id, float max_speed);

    /// 立即停止实体移动（velocity 清零）。
    core::Result<void> Stop(EntityId id);

    // ---- 状态访问（测试 / 集成 / 调度用） ----

    /// 注册实体初始移动状态（spawn / 进入场景时调用；重复注册覆盖）。
    core::Result<void> Register(EntityId id, const MovementState& init);

    /// 取可变状态指针（测试直接设置 velocity/pos/flags）。
    /// 注意：core::Result<T> 内部为 std::variant<T, Error>，不支持引用类型，
    /// 故此处返回指针而非 MovementState&（生命周期由 states_ 持有，非 owning）。
    core::Result<MovementState*> StateOf(EntityId id);

    /// 设置速度向量（测试 / 运动意图；积分用）。
    core::Result<void> SetVelocity(EntityId id, Vec3 v);

    /// 运行期统计快照（§6 反作弊指标：拒绝原因分布 / 纠偏数 / 移动数）。
    MovementStats Stats() const noexcept { return stats_; }

    /// 当前配置（测试 / 集成读取 max_speed / tick_dt 等）。
    const MovementConfig& GetConfig() const noexcept { return cfg_; }

    /// 重置统计（benchmark / 分段采集）。
    void ResetStats() noexcept { stats_ = MovementStats{}; }

    /// 当前管理实体数。
    std::size_t EntityCount() const noexcept { return states_.size(); }

private:
    /// 取得或创建状态（首次命令时懒注册）。
    MovementState& EnsureState(EntityId id);

    /// 把权威位置写回实体并触发 AOI Move + 发布视图变化事件。
    void CommitPosition(EntityId id, const Position& pos, const SceneContext& ctx);

    /// 钳制超速位移：沿 origin→to 方向缩放到 step*tolerance。
    static Position ClampSpeed(const Position& origin, const Position& to,
                               const MovementConfig& cfg) noexcept;
    /// 钳制到世界边界盒。
    static Position ClampBounds(const Position& to, const MovementConfig& cfg) noexcept;

    MovementConfig cfg_;
    aoi::IAoi* aoi_{nullptr};
    std::unordered_map<EntityId, MovementState> states_;
    MovementStats stats_;
};

}  // namespace mmo::game::movement
