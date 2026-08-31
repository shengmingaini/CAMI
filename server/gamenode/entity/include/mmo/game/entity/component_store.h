#pragma once

/// TASK-011 · 组件存储（§8 / §15.3 / §15.5）。
///
/// 第一版用「每类型一个稀疏集合（sparse set / EnTT 风格）」而非每实体一个 map：
///   - dense：连续存储组件实例（遍历局部性好，§8）；
///   - sparse[index]：实体槽位索引 -> dense 下标（O(1) 取组件）；
///   - 增删用 swap-remove，O(1) 且稳定。
/// 类型擦除基类 IComponentStorage 供 EntityManager 持有异质 store 并批量 Detach。

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "mmo/game/entity/entity.h"  // IComponent / ComponentTypeId / ComponentMask / Entity

namespace mmo::game {

/// 稀疏集合中的空槽哨兵。
inline constexpr std::uint32_t kNullSlot = 0xFFFFFFFFu;

/// 每组件类型分配一个稠密 ID（进程级；首个调用该类型的 ComponentTypeIdFor 时分配）。
/// 用原子计数器保证多线程首次注册安全，且 ID 连续（适合做掩码位）。
inline ComponentTypeId NextComponentTypeId() noexcept {
    static std::atomic<std::uint16_t> next{0};
    return static_cast<ComponentTypeId>(next.fetch_add(1, std::memory_order_relaxed));
}

template <typename C>
ComponentTypeId ComponentTypeIdFor() noexcept {
    static const ComponentTypeId id = NextComponentTypeId();
    return id;
}

/// 类型擦除的组件存储基类：供 EntityManager 持有异质 store 并批量 Detach（§15.7）。
class IComponentStorage {
public:
    virtual ~IComponentStorage() = default;

    /// 回收某实体在该类型下的组件（OnDetached + 析构），不存在则 no-op。
    virtual void RemoveForEntity(std::uint32_t entity_index) = 0;

    /// 清空全部组件。
    virtual void Clear() = 0;

    /// 当前挂载该类型组件的实体数。
    virtual std::size_t Count() const noexcept = 0;
};

/// 单组件类型的稀疏集合存储（§8 / §15.3）。
template <typename C>
class ComponentStore : public IComponentStorage {
public:
    /// 为实体 entity_index 构造一个组件实例（Args 转发给 C 的构造函数）。
    /// 返回组件指针（稳定，dense 不重排直到该槽被 swap-remove）。
    template <typename... Args>
    C* Emplace(std::uint32_t entity_index, Entity& owner, Args&&... args) {
        const std::size_t dense_idx = dense_.size();
        dense_.emplace_back(std::forward<Args>(args)...);
        dense_to_entity_.push_back(entity_index);
        EnsureSparse(entity_index) = static_cast<std::uint32_t>(dense_idx);
        C* c = &dense_.back();
        c->OnAttached(owner);
        return c;
    }

    /// O(1) 取组件；未挂载返回 nullptr（类型安全，§20.4）。
    C* Get(std::uint32_t entity_index) noexcept {
        if (entity_index >= sparse_.size()) {
            return nullptr;
        }
        const std::uint32_t idx = sparse_[entity_index];
        if (idx == kNullSlot) {
            return nullptr;
        }
        return &dense_[idx];
    }

    const C* Get(std::uint32_t entity_index) const noexcept {
        if (entity_index >= sparse_.size()) {
            return nullptr;
        }
        const std::uint32_t idx = sparse_[entity_index];
        if (idx == kNullSlot) {
            return nullptr;
        }
        return &dense_[idx];
    }

    /// 卸载某实体的该类型组件（swap-remove，O(1)）。OnDetached 由调用方先触发。
    void RemoveForEntity(std::uint32_t entity_index) override {
        if (entity_index >= sparse_.size()) {
            return;
        }
        const std::uint32_t idx = sparse_[entity_index];
        if (idx == kNullSlot) {
            return;
        }
        const std::uint32_t last =
            static_cast<std::uint32_t>(dense_.size() - 1);
        if (idx != last) {
            dense_[idx] = std::move(dense_[last]);
            dense_to_entity_[idx] = dense_to_entity_[last];
            sparse_[dense_to_entity_[idx]] = idx;
        }
        dense_.pop_back();
        dense_to_entity_.pop_back();
        sparse_[entity_index] = kNullSlot;
    }

    void Clear() override {
        dense_.clear();
        dense_to_entity_.clear();
        sparse_.clear();
    }

    std::size_t Count() const noexcept override { return dense_.size(); }

    /// 遍历辅助（供 EntityManager::Each 使用）。
    C& At(std::size_t dense_idx) noexcept { return dense_[dense_idx]; }
    const C& At(std::size_t dense_idx) const noexcept { return dense_[dense_idx]; }
    std::uint32_t EntityIndexAt(std::size_t dense_idx) const noexcept {
        return dense_to_entity_[dense_idx];
    }

private:
    std::uint32_t& EnsureSparse(std::uint32_t entity_index) {
        if (entity_index >= sparse_.size()) {
            sparse_.resize(static_cast<std::size_t>(entity_index) + 1, kNullSlot);
        }
        return sparse_[entity_index];
    }

    std::vector<C> dense_;                   // 组件实例（连续，遍历友好）
    std::vector<std::uint32_t> dense_to_entity_;  // dense[i] -> 实体槽位索引
    std::vector<std::uint32_t> sparse_;     // 实体槽位索引 -> dense 下标（或 kNullSlot）
};

}  // namespace mmo::game
