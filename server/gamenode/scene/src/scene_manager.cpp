// server/gamenode/scene/src/scene_manager.cpp — TASK-012 §15.4 / §15.9 / §17 / §19
//
// SceneManager 实现：Scene 表读写锁保护，TickAll 单写者互斥（禁止并发 Tick 同一 Scene），
// 并发创建同 Id 返回 VERSION_CONFLICT。销毁场景在锁外发布事件，避免回调重入 scenes_mu_。

#include "mmo/game/scene/scene_manager.h"

#include "mmo/game/scene/scene_events.h"

namespace mmo::game {

SceneManager::SceneManager(EntityManager& entities, core::EventBus& events,
                           core::Scheduler& scheduler, core::Arena& arena)
    : entities_(entities), events_(events), scheduler_(scheduler), arena_(arena) {}

core::Result<Scene*> SceneManager::Create(SceneId id, SceneType type, NodeId owner) {
    std::unique_lock<std::shared_mutex> wl(scenes_mu_);
    if (scenes_.count(id)) {
        return core::Result<Scene*>::Fail(core::Error(core::ErrorCode::VERSION_CONFLICT,
            "scene id already exists", core::domain::kScene));
    }
    auto s = std::make_unique<Scene>(id, type, owner, entities_, events_);
    Scene* ptr = s.get();
    scenes_[id] = std::move(s);
    // Scene 构造已发布 SceneCreated，此处不重复发布。
    return core::Result<Scene*>::Ok(ptr);
}

core::Result<Scene*> SceneManager::Find(SceneId id) noexcept {
    std::shared_lock<std::shared_mutex> rl(scenes_mu_);
    auto it = scenes_.find(id);
    if (it == scenes_.end()) {
        return core::Result<Scene*>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
            "scene not found", core::domain::kScene));
    }
    return core::Result<Scene*>::Ok(it->second.get());
}

core::Result<void> SceneManager::Destroy(SceneId id) {
    // 与 TickAll 共享 tick_mu_：持有期间禁止任何 Tick 中途销毁（§21 禁止 Tick 内同步销毁）。
    std::unique_lock<std::mutex> tick_lock(tick_mu_);
    std::unique_ptr<Scene> s;
    SceneId sid{0};
    SceneType t{SceneType::World};
    NodeId owner{0};
    {
        std::unique_lock<std::shared_mutex> wl(scenes_mu_);
        auto it = scenes_.find(id);
        if (it == scenes_.end()) {
            return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                "scene not found", core::domain::kScene));
        }
        s = std::move(it->second);
        scenes_.erase(it);
        sid = s->Id();
        t = s->Type();
        owner = s->OwnerNode();
    }
    // 锁外发布：避免订阅者回调里重入 scenes_mu_ 造成死锁；Destroy 已移出表，快照指针仍有效。
    (void)events_.Publish(SceneDestroyed{sid, t, owner});
    return core::Result<void>::Ok();
}

core::Result<void> SceneManager::TickAll(core::SteadyTime now) {
    // 单写者互斥：保证任意时刻只有一个 TickAll 在执行，禁止并发 Tick 同一 Scene（§9 / §21）。
    std::unique_lock<std::mutex> tick_lock(tick_mu_);
    std::vector<Scene*> snap;
    {
        std::shared_lock<std::shared_mutex> rl(scenes_mu_);
        snap.reserve(scenes_.size());
        for (auto& kv : scenes_) {
            snap.push_back(kv.second.get());
        }
    }
    // 快照外 Tick：Tick 不持 scenes_mu_，Destroy 不会阻塞 TickAll 太久（仅等待 tick_mu_）。
    for (Scene* s : snap) {
        SceneContext ctx(s->Id(), s->Type(), s->OwnerNode(), now, s->TickNumber(),
                         entities_, events_, scheduler_, arena_);
        (void)s->Tick(ctx);
    }
    return core::Result<void>::Ok();
}

std::vector<Scene*> SceneManager::All() const {
    std::shared_lock<std::shared_mutex> rl(scenes_mu_);
    std::vector<Scene*> out;
    out.reserve(scenes_.size());
    for (auto& kv : scenes_) {
        out.push_back(kv.second.get());
    }
    return out;
}

std::size_t SceneManager::Count() const noexcept {
    std::shared_lock<std::shared_mutex> rl(scenes_mu_);
    return scenes_.size();
}

}  // namespace mmo::game
