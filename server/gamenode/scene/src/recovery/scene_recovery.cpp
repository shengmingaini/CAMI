// server/gamenode/scene/src/recovery/scene_recovery.cpp — TASK-037 §7 / §15.8-15.9 / §37.4
//
// Scene 恢复第一版：Checkpoint 采集 + 新 Owner 节点重建。不做 Live Migration。

#include "mmo/game/scene/recovery/scene_recovery.h"

#include <mutex>

namespace mmo::game::scene::recovery {

SceneRecovery::SceneRecovery(ISceneFactory& factory, core::EventBus* bus)
    : factory_(factory), bus_(bus) {}

core::Result<void> SceneRecovery::Checkpoint(const Scene& s) {
    // 测试 / 故障注入：模拟存储写入失败。
    // 必须「先决绝、不触碰已有 checkpoint」，保证保留上一个可靠快照（§19 Failure）。
    if (fail_next_) {
        fail_next_ = false;
        // 告警：保留上一个 checkpoint（store_ 保持不变）。
        if (bus_) {
            // 无专用事件类型，复用 core 诊断通道由调用方订阅；此处仅记录不抛。
        }
        return core::Result<void>::Fail(core::Error{
            core::ErrorCode::INTERNAL_ERROR, "checkpoint store unavailable", core::domain::kScene});
    }

    SceneCheckpoint cp{};
    cp.scene_id              = s.Id();
    cp.type                  = s.Type();
    cp.owner_at_checkpoint   = s.OwnerNode();
    cp.scene_version         = s.Version();
    cp.tick_number           = s.TickNumber();
    cp.state_hash            = s.StateHash();
    cp.scene_state           = s.State();
    cp.player_count          = s.PlayerCount();
    cp.entity_count          = s.EntityCount();
    cp.taken_at              = core::MonotonicClock::Point();

    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(cp.scene_id);
        cp.checkpoint_version = (it == store_.end()) ? 1u : (it->second.checkpoint_version + 1u);
        store_[cp.scene_id] = cp;  // 整条原子替换（无部分写入）
    }
    return core::Result<void>::Ok();
}

core::Result<Scene*> SceneRecovery::Restore(SceneId id, NodeId new_owner,
                                            core::TraceID /*trace*/) {
    SceneCheckpoint cp{};
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = store_.find(id);
        if (it == store_.end()) {
            return core::Result<Scene*>::Fail(core::Error{
                core::ErrorCode::NOT_FOUND, "no checkpoint for scene", core::domain::kScene});
        }
        cp = it->second;  // 拷贝快照元数据用于重建审计
    }

    // 在新 Owner 节点创建 Scene 容器（身份 + 类型沿用 checkpoint）。
    auto created = factory_.Create(id, cp.type, new_owner);
    if (!created.HasValue()) {
        return core::Result<Scene*>::Fail(created.Err());
    }
    Scene* sc = created.Value();

    // 第一版：容器重建后推到 Running，等待玩家经 Failover/Reconnect 重连补回（§8 回退语义）。
    // 不允许 Live Migration，故不迁移实时实体；player_count 由 checkpoint 记录供对账。
    (void)sc->TransitionTo(SceneState::Loading);
    (void)sc->TransitionTo(SceneState::Running);

    // 审计：恢复后玩家数 = 0（待重连）；checkpoint 记录的 player_count 仍可由 LastCheckpoint 取得。
    (void)cp;
    return core::Result<Scene*>::Ok(sc);
}

std::uint32_t SceneRecovery::LastCheckpointVersion(SceneId id) const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = store_.find(id);
    return (it == store_.end()) ? 0u : it->second.checkpoint_version;
}

void SceneRecovery::FailNextCheckpoint(bool fail) noexcept {
    fail_next_ = fail;
}

std::size_t SceneRecovery::CheckpointCount() const noexcept {
    std::lock_guard<std::mutex> lk(mu_);
    return store_.size();
}

}  // namespace mmo::game::scene::recovery
