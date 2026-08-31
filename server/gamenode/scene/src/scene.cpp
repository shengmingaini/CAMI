// server/gamenode/scene/src/scene.cpp — TASK-012 §15.2 / §15.5 / §15.6 / §15.7 / §15.8
//
// Scene 实现：五状态机、Enter/Leave（绑定/解绑 Avatar，延迟销毁）、Tick（每 N Tick
// 算增量哈希）、StateHash 确定性滚动哈希。热路径禁止任何外部 IO（§10 / §21）。

#include "mmo/game/scene/scene.h"

#include <algorithm>
#include <vector>

#include "mmo/game/scene/scene_events.h"

namespace mmo::game {

namespace {
// FNV-1a 64 位滚动哈希（确定性，跨进程稳定），用于 StateHash（§15.6 / §16）。
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
inline void Fnvmix(std::uint64_t& h, std::uint64_t v) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= kFnvPrime;
    }
}
}  // namespace

// ---------------------------------------------------------------------------
// 构造 / 访问器
// ---------------------------------------------------------------------------
Scene::Scene(SceneId id, SceneType type, NodeId owner,
             EntityManager& entities, core::EventBus& events)
    : id_(id), owner_node_(owner), type_(type), state_(SceneState::Creating),
      entities_(entities), events_(events),
      max_players_(kDefaultMaxPlayers), max_entities_(kDefaultMaxEntities),
      state_hash_period_(kDefaultHashPeriod) {
    // 创建即进入 Creating 态并发布 SceneCreated（§15.2）
    (void)events_.Publish(SceneCreated{id_, type_, owner_node_});
}

SceneId Scene::Id() const noexcept { return id_; }
SceneType Scene::Type() const noexcept { return type_; }
SceneState Scene::State() const noexcept { return state_; }
std::uint32_t Scene::Version() const noexcept { return version_; }
std::uint64_t Scene::TickNumber() const noexcept { return tick_number_; }
std::uint64_t Scene::StateHash() const noexcept { return state_hash_; }
NodeId Scene::OwnerNode() const noexcept { return owner_node_; }

std::size_t Scene::PlayerCount() const noexcept { return players_.size(); }
std::size_t Scene::EntityCount() const noexcept { return entity_count_; }

// ---------------------------------------------------------------------------
// §15.5 Enter / Leave
// ---------------------------------------------------------------------------
core::Result<void> Scene::Enter(PlayerId pid, EntityId avatar) {
    // 仅 Running 态接纳玩家；Draining 期间禁止新玩家进入（§17 断言拒绝）
    if (state_ != SceneState::Running) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
            "scene not accepting players", core::domain::kScene));
    }
    if (players_.size() >= max_players_ || entity_count_ >= max_entities_) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::BUSY,
            "scene capacity reached", core::domain::kScene));
    }
    if (players_.count(pid)) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
            "player already in scene", core::domain::kScene));
    }
    players_[pid] = avatar;
    ++entity_count_;
    (void)events_.Publish(ScenePlayerEntered{id_, pid, avatar});
    return core::Result<void>::Ok();
}

core::Result<void> Scene::Leave(PlayerId pid, LeaveReason reason) {
    auto it = players_.find(pid);
    if (it == players_.end()) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
            "player not in scene", core::domain::kScene));
    }
    const EntityId avatar = it->second;
    players_.erase(it);
    if (entity_count_ > 0) --entity_count_;
    // 延迟销毁 Avatar：EntityManager::Destroy 仅逻辑死亡 + 世代 +1，物理回收推迟到
    // 宿主线程 FlushDeferred（Tick 边界），避免在 Leave 调用点同步销毁（§15.5 / §21）。
    (void)entities_.Destroy(avatar);
    (void)events_.Publish(ScenePlayerLeft{id_, pid, avatar, reason});
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// §7 / §15 Tick（由 SceneManager::TickAll 驱动；本任务只留接口）
// ---------------------------------------------------------------------------
core::Result<void> Scene::Tick(const SceneContext& ctx) {
    ++tick_number_;
    // 默认每 60 Tick 算一次增量哈希（§15.6 / §22）。StateHash 计算有界、远快于 1 Tick
    // 预算，失败路径（§19 超时跳过）在此实现中不会触发；若未来哈希变重，应在此 try 保护
    // 并递增 skip 计数，禁止拖慢 Tick。
    if (tick_number_ % state_hash_period_ == 0) {
        (void)ComputeStateHash();
    }
    (void)ctx;
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// §8 五状态机
// ---------------------------------------------------------------------------
bool Scene::IsLegalTransition(SceneState from, SceneState to) const noexcept {
    switch (from) {
        case SceneState::Creating:   return to == SceneState::Loading || to == SceneState::Destroying;
        case SceneState::Loading:    return to == SceneState::Running || to == SceneState::Destroying;
        case SceneState::Running:    return to == SceneState::Draining || to == SceneState::Destroying;
        case SceneState::Draining:   return to == SceneState::Destroying;
        case SceneState::Destroying: return false;  // 终态，不可再转移
    }
    return false;
}

core::Result<void> Scene::TransitionTo(SceneState to) {
    if (!IsLegalTransition(state_, to)) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
            "illegal scene state transition", core::domain::kScene));
    }
    state_ = to;
    ++version_;  // 版本号随每次合法状态变更递增（供迁移/乐观校验，§7）
    PublishStateEvent(to);
    return core::Result<void>::Ok();
}

void Scene::PublishStateEvent(SceneState to) {
    switch (to) {
        case SceneState::Loading:    (void)events_.Publish(SceneLoaded{id_, type_, owner_node_}); break;
        case SceneState::Running:    (void)events_.Publish(SceneRunning{id_, type_, owner_node_}); break;
        case SceneState::Draining:   (void)events_.Publish(SceneDraining{id_, type_, owner_node_}); break;
        case SceneState::Destroying: (void)events_.Publish(SceneDestroyed{id_, type_, owner_node_}); break;
        default: break;  // Creating 在构造期已发布 SceneCreated
    }
}

// ---------------------------------------------------------------------------
// §15.6 / §16 增量滚动哈希（确定性可复现）
// ---------------------------------------------------------------------------
core::Result<void> Scene::ComputeStateHash() {
    // 确定性：对玩家 ID 集合排序后逐元素 FNV，避免 unordered_map 迭代序随机（libstdc++
    // 默认带随机种子）导致同操作序列产生不同哈希（§16 确定性要求）。
    std::uint64_t h = kFnvOffset;
    Fnvmix(h, tick_number_);
    Fnvmix(h, static_cast<std::uint64_t>(players_.size()));
    Fnvmix(h, static_cast<std::uint64_t>(entity_count_));
    std::vector<PlayerId> ps;
    ps.reserve(players_.size());
    for (const auto& kv : players_) {
        ps.push_back(kv.first);
    }
    std::sort(ps.begin(), ps.end());
    for (PlayerId pid : ps) {
        Fnvmix(h, static_cast<std::uint64_t>(pid));
    }
    state_hash_ = h;
    return core::Result<void>::Ok();
}

}  // namespace mmo::game
