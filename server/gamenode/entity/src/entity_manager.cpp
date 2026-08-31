// server/gamenode/entity/src/entity_manager.cpp — TASK-011 §15.4 / §15.6 / §15.7
//
// 仅实现非模板方法（模板方法在 entity_manager.h 内联）。
// 依赖：TASK-004 ObjectPool（实体池化）、TASK-007 EventBus（生命周期事件）。

#include "mmo/game/entity/entity_manager.h"

namespace mmo::game {

core::Result<Entity*> EntityManager::Create(EntityType type, SceneId scene,
                                            const Position& pos) {
    const std::uint32_t index = AcquireIndex();
    if (index == kNullSlot) {
        return core::Result<Entity*>::Fail(core::Error(
            core::ErrorCode::BUSY, "entity capacity reached", core::domain::kScene));
    }

    Entity* e = pool_.Acquire();  // 默认构造（池化，热路径无分配）
    const std::uint32_t gen = slot_gen_[index];
    const EntityId id = MakeEntityId(index, gen);

    e->SetOwner(this);
    e->BindIdentity(id, type, scene);
    e->SetPos(pos);
    e->SetAlive(true);
    e->Components() = 0;

    index_to_entity_[index] = e;
    ++type_counts_[static_cast<std::size_t>(type)];
    ++alive_count_;
    ++create_count_;

    if (bus_ != nullptr) {
        (void)bus_->Publish(EntityCreated{id, type, scene});
    }
    return core::Result<Entity*>::Ok(e);
}

core::Result<void> EntityManager::Destroy(EntityId id) {
    Entity* e = Find(id);
    if (e == nullptr) {
        // 已销毁 / 不存在：幂等返回 NOT_FOUND（§19，禁止重复回收导致槽位错乱）。
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "entity not found or already destroyed",
            core::domain::kScene));
    }

    // 立即逻辑死亡 + 世代 +1：旧 id 即刻失效（防 ABA，且 Tick 中途访问返回 nullptr 不悬垂）。
    e->SetAlive(false);
    const std::uint32_t index = EntityIndex(id);
    slot_gen_[index] += 1;

    // 物理回收推迟到 FlushDeferred（Tick 边界）。
    deferred_.push_back(id);
    return core::Result<void>::Ok();
}

Entity* EntityManager::Find(EntityId id) noexcept {
    const std::uint32_t index = EntityIndex(id);
    if (index >= index_to_entity_.size()) {
        return nullptr;
    }
    Entity* e = index_to_entity_[index];
    if (e == nullptr) {
        return nullptr;
    }
    if (!e->Alive()) {
        return nullptr;
    }
    if (EntityGeneration(id) != slot_gen_[index]) {
        return nullptr;  // 世代不匹配（已回收或销毁）→ 防 ABA
    }
    return e;
}

const Entity* EntityManager::Find(EntityId id) const noexcept {
    const std::uint32_t index = EntityIndex(id);
    if (index >= index_to_entity_.size()) {
        return nullptr;
    }
    const Entity* e = index_to_entity_[index];
    if (e == nullptr) {
        return nullptr;
    }
    if (!e->Alive()) {
        return nullptr;
    }
    if (EntityGeneration(id) != slot_gen_[index]) {
        return nullptr;
    }
    return e;
}

void EntityManager::FlushDeferred() noexcept {
    if (deferred_.empty()) {
        return;
    }
    for (EntityId id : deferred_) {
        const std::uint32_t index = EntityIndex(id);
        Entity* e = index_to_entity_[index];
        if (e == nullptr) {
            continue;
        }

        // 1) 卸载全部组件（OnDetached + 析构 + 发布 ComponentDetached）。
        DetachAll(*e);

        // 2) 发布 EntityDestroyed（物理回收时刻 = Tick 边界，§15.7）。
        if (bus_ != nullptr) {
            (void)bus_->Publish(EntityDestroyed{id, e->Type(), e->Scene()});
        }

        // 3) 物理回收：放回池、清空索引、归还槽位。
        index_to_entity_[index] = nullptr;
        pool_.Release(e);
        ReleaseIndex(index);
        --type_counts_[static_cast<std::size_t>(e->Type())];
        --alive_count_;
        ++destroy_count_;
    }
    deferred_.clear();
}

void EntityManager::DetachAll(Entity& e) noexcept {
    ComponentMask mask = e.Components();
    while (mask != 0) {
        const std::uint32_t bit =
            static_cast<std::uint32_t>(__builtin_ctzll(mask));
        mask &= ~(static_cast<ComponentMask>(1u) << bit);
        const std::size_t tidx = static_cast<std::size_t>(bit);
        if (tidx < stores_.size() && stores_[tidx] != nullptr) {
            stores_[tidx]->RemoveForEntity(EntityIndex(e.Id()));
        }
    }
}

}  // namespace mmo::game
