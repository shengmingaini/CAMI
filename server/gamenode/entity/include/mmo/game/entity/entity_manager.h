#pragma once

/// TASK-011 · EntityManager（§7 / §15.4 / §15.6 / §15.7 / §15.8）。
///
/// 职责：实体创建 / 销毁 / 查找 / 组件挂载卸载 / 延迟销毁 / 指标。
/// 依赖：TASK-004 ObjectPool（实体池化）、TASK-007 EventBus（生命周期事件）。
///
/// SlotMap（§8）：index_to_entity_[index] 指向池中的 Entity；slot_gen_[index] 为世代号。
/// 销毁时立即逻辑死亡 + 世代 +1（旧 id 即刻失效，防 ABA 与 Tick 中途悬垂），物理回收
/// 推迟到 FlushDeferred（Tick 边界）统一执行（§15.7 / §19）。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/memory/object_pool.h"

#include "mmo/game/entity/component_store.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_events.h"
#include "mmo/game/entity/entity_id.h"

namespace mmo::game {

class EntityManager {
public:
    /// bus：可选，非空时发布生命周期事件（§15.6）；max_entities：实体数硬上限，
    /// 超出返回 BUSY 而非 OOM（§19 / §22）。
    explicit EntityManager(core::EventBus* bus = nullptr,
                           std::size_t max_entities = kDefaultMaxEntities)
        : bus_(bus), max_entities_(max_entities),
          pool_(kPoolPrewarm), type_counts_(kEntityTypeSlots, 0) {}

    ~EntityManager() = default;

    EntityManager(const EntityManager&) = delete;
    EntityManager& operator=(const EntityManager&) = delete;

    // ---- 创建 / 销毁 / 查找（§7）----

    /// 创建实体：从 ObjectPool 取槽，分配 index，生成 EntityId（index|generation）。
    core::Result<Entity*> Create(EntityType type, SceneId scene,
                                 const Position& pos);

    /// 延迟销毁：立即逻辑死亡（alive=false + 世代 +1 使旧 id 失效），物理回收推迟到
    /// FlushDeferred（Tick 边界）。重复 Destroy 同一 id → NOT_FOUND（§19 幂等）。
    core::Result<void> Destroy(EntityId id);

    /// O(1) 查找：index 越界 / 世代不匹配 / 已死 → nullptr（防 ABA，§20.1）。
    Entity* Find(EntityId id) noexcept;
    const Entity* Find(EntityId id) const noexcept;

    /// 遍历某组件类型的全部 (实体, 组件)。dense 连续，缓存友好（§15.4 / §8）。
    template <typename C>
    void Each(std::function<void(Entity&, C&)> fn);

    /// 每类型存活数 / 总存活数。
    std::size_t Count(EntityType type) const noexcept {
        return type_counts_[static_cast<std::size_t>(type)];
    }
    std::size_t AliveCount() const noexcept { return alive_count_; }

    /// 当前已分配槽位数（capacity）。
    std::size_t Capacity() const noexcept { return index_to_entity_.size(); }

    // ---- Tick 边界（§15.7 / §19）----

    /// 回收所有 pending Destroy 的实体（释放槽位 + 组件 + 发布 EntityDestroyed）。
    /// 由宿主线程在 Tick 末尾调用，禁止在 Tick 中途调用。
    void FlushDeferred() noexcept;

    // ---- 指标（§15.8 / §22）----

    std::size_t CreateCount() const noexcept { return create_count_; }
    std::size_t DestroyCount() const noexcept { return destroy_count_; }
    std::size_t DeferredCount() const noexcept { return deferred_.size(); }

    /// 单实体结构开销（字节）：Entity 本身 + 每槽辅助数组（世代 + 空闲表）。
    /// 不含组件（组件存于稀疏数组，按类型聚合，不计入单实体预算，§22）。
    static constexpr std::size_t MemoryBytesPerEntity() noexcept {
        return sizeof(Entity) + sizeof(std::uint32_t) + sizeof(std::uint32_t);
    }

    // ---- 组件操作（§7 / §15.5），模板定义见下 ----

    template <typename C, typename... Args>
    C* AttachComponent(Entity& e, Args&&... args);

    template <typename C>
    C* GetComponent(Entity& e) noexcept;

    template <typename C>
    const C* GetComponent(const Entity& e) const noexcept;

    template <typename C>
    bool DetachComponent(Entity& e) noexcept;

private:
    static constexpr std::size_t kDefaultMaxEntities = 4'000'000;
    static constexpr std::size_t kEntityTypeSlots = 8;  // EntityType 取值 1..6
    static constexpr std::size_t kPoolPrewarm = 4096;

    Entity* IndexToEntity(std::uint32_t index) const noexcept {
        if (index >= index_to_entity_.size()) {
            return nullptr;
        }
        return index_to_entity_[index];
    }

    /// 从空闲表或新槽取一个 index；满则返回 kNullSlot（调用方转 BUSY）。
    std::uint32_t AcquireIndex();
    void ReleaseIndex(std::uint32_t index) noexcept;

    template <typename C>
    ComponentStore<C>* GetStore();

    /// Flush 时卸载某实体的全部组件（OnDetached + 析构 + 发布 ComponentDetached）。
    void DetachAll(Entity& e) noexcept;

    core::EventBus* bus_;
    const std::size_t max_entities_;
    core::ObjectPool<Entity> pool_;
    std::vector<Entity*> index_to_entity_;        // index -> Entity*
    std::vector<std::uint32_t> slot_gen_;          // index -> 世代号
    std::vector<std::uint32_t> free_indices_;      // 可回收槽位栈
    std::vector<std::size_t> type_counts_;         // 每 EntityType 存活数
    std::vector<std::unique_ptr<IComponentStorage>> stores_;  // 每组件类型一个
    std::vector<EntityId> deferred_;               // 待物理回收的实体
    std::size_t create_count_ = 0;
    std::size_t destroy_count_ = 0;
    std::size_t alive_count_ = 0;
};

// ===========================================================================
// 模板方法定义（依赖 ComponentStore / ComponentTypeIdFor，必须于头文件内联）
// ===========================================================================

inline std::uint32_t EntityManager::AcquireIndex() {
    if (!free_indices_.empty()) {
        const std::uint32_t idx = free_indices_.back();
        free_indices_.pop_back();
        return idx;
    }
    const std::size_t new_idx = index_to_entity_.size();
    if (new_idx >= max_entities_) {
        return kNullSlot;
    }
    index_to_entity_.push_back(nullptr);
    slot_gen_.push_back(1);  // 世代从 1 起，使 EntityId{0} 永不为有效实体
    return static_cast<std::uint32_t>(new_idx);
}

inline void EntityManager::ReleaseIndex(std::uint32_t index) noexcept {
    free_indices_.push_back(index);
}

template <typename C>
ComponentStore<C>* EntityManager::GetStore() {
    const std::size_t tidx = static_cast<std::size_t>(
        static_cast<std::uint16_t>(ComponentTypeIdFor<C>()));
    if (tidx >= stores_.size()) {
        return nullptr;
    }
    if (stores_[tidx] == nullptr) {
        return nullptr;
    }
    return static_cast<ComponentStore<C>*>(stores_[tidx].get());
}

template <typename C, typename... Args>
C* EntityManager::AttachComponent(Entity& e, Args&&... args) {
    const ComponentTypeId tid = ComponentTypeIdFor<C>();
    const std::size_t tidx = static_cast<std::size_t>(
        static_cast<std::uint16_t>(tid));

    // 已挂载同类型 → 幂等返回既有（§19：类型不匹配返回 nullptr，同类型不报错）。
    if ((e.Components() & ComponentBit(tid)) != 0) {
        auto* store = GetStore<C>();
        if (store != nullptr) {
            return store->Get(EntityIndex(e.Id()));
        }
    }

    if (tidx >= stores_.size()) {
        stores_.resize(tidx + 1);
    }
    if (stores_[tidx] == nullptr) {
        stores_[tidx] = std::make_unique<ComponentStore<C>>();
    }
    auto* store = static_cast<ComponentStore<C>*>(stores_[tidx].get());

    C* c = store->Emplace(EntityIndex(e.Id()), e, std::forward<Args>(args)...);
    e.Components() |= ComponentBit(tid);
    if (bus_ != nullptr) {
        (void)bus_->Publish(ComponentAttached{e.Id(), tid});
    }
    return c;
}

template <typename C>
C* EntityManager::GetComponent(Entity& e) noexcept {
    const ComponentTypeId tid = ComponentTypeIdFor<C>();
    if ((e.Components() & ComponentBit(tid)) == 0) {
        return nullptr;
    }
    auto* store = GetStore<C>();
    if (store == nullptr) {
        return nullptr;
    }
    return store->Get(EntityIndex(e.Id()));
}

template <typename C>
const C* EntityManager::GetComponent(const Entity& e) const noexcept {
    const ComponentTypeId tid = ComponentTypeIdFor<C>();
    if ((e.Components() & ComponentBit(tid)) == 0) {
        return nullptr;
    }
    const std::size_t tidx = static_cast<std::size_t>(
        static_cast<std::uint16_t>(tid));
    if (tidx >= stores_.size() || stores_[tidx] == nullptr) {
        return nullptr;
    }
    const auto* store =
        static_cast<const ComponentStore<C>*>(stores_[tidx].get());
    return store->Get(EntityIndex(e.Id()));
}

template <typename C>
bool EntityManager::DetachComponent(Entity& e) noexcept {
    const ComponentTypeId tid = ComponentTypeIdFor<C>();
    if ((e.Components() & ComponentBit(tid)) == 0) {
        return false;  // 未挂载
    }
    const std::size_t tidx = static_cast<std::size_t>(
        static_cast<std::uint16_t>(tid));
    const std::uint32_t ei = EntityIndex(e.Id());
    C* c = nullptr;
    if (tidx < stores_.size() && stores_[tidx] != nullptr) {
        auto* store = static_cast<ComponentStore<C>*>(stores_[tidx].get());
        c = store->Get(ei);
        if (c != nullptr) {
            c->OnDetached(e);
        }
        store->RemoveForEntity(ei);
    }
    e.Components() &= ~ComponentBit(tid);
    if (bus_ != nullptr && c != nullptr) {
        (void)bus_->Publish(ComponentDetached{e.Id(), tid});
    }
    return true;
}

template <typename C>
void EntityManager::Each(std::function<void(Entity&, C&)> fn) {
    auto* store = GetStore<C>();
    if (store == nullptr) {
        return;
    }
    const std::size_t n = store->Count();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t ei = store->EntityIndexAt(i);
        Entity* e = IndexToEntity(ei);
        if (e != nullptr) {
            fn(*e, store->At(i));
        }
    }
}

// ===========================================================================
// Entity 的成员模板：转调 EntityManager（需 EntityManager 完整定义，故置于此处）
// ===========================================================================

template <typename C, typename... Args>
C* Entity::AddComponent(Args&&... args) {
    if (mgr_ == nullptr) {
        return nullptr;
    }
    return mgr_->AttachComponent<C>(*this, std::forward<Args>(args)...);
}

template <typename C>
C* Entity::TryGet() noexcept {
    if (mgr_ == nullptr) {
        return nullptr;
    }
    return mgr_->GetComponent<C>(*this);
}

template <typename C>
const C* Entity::TryGet() const noexcept {
    if (mgr_ == nullptr) {
        return nullptr;
    }
    return mgr_->GetComponent<C>(*this);
}

template <typename C>
bool Entity::RemoveComponent() noexcept {
    if (mgr_ == nullptr) {
        return false;
    }
    return mgr_->DetachComponent<C>(*this);
}

}  // namespace mmo::game
