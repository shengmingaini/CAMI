#pragma once

/// TASK-011 · Entity 结构、组件抽象基类与组件类型 ID（§7 / §8）。
///
/// Entity 固定大小、可池化（§15.2）：仅持有 id / type / scene / position / 组件掩码 /
/// 版本号 + 一个指向所属 EntityManager 的弱指针，组件数据不内联（存于稀疏数组，
/// 见 component_store.h），故单实体内存 < 256B（§22）。

#include <cstdint>
#include <functional>
#include <string>

#include "mmo/game/entity/entity_id.h"

namespace mmo::game {

/// 实体类型（§7）。取值固定，序列化后跨进程稳定。
enum class EntityType : std::uint8_t {
    Player = 1,
    Monster = 2,
    Npc = 3,
    Projectile = 4,
    Effect = 5,
    Item = 6,
};

/// 实体位置（§7）。平面 + yaw，4 个 float 共 16 字节。
struct Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
};

/// 组件类型 ID（强类型 uint16，每组件类型进程内唯一，由 component_store.h 的
/// ComponentTypeIdFor<C>() 在首次使用时分配；ID 稠密，适合做掩码位）。
enum class ComponentTypeId : std::uint16_t {};

/// 组件掩码：单实体已挂载的组件类型集合（≤64 种，覆盖 MMORPG 全部组件类型绰绰有余）。
using ComponentMask = std::uint64_t;

/// 取某组件类型的掩码位（§8）。
inline constexpr ComponentMask ComponentBit(ComponentTypeId id) noexcept {
    return static_cast<ComponentMask>(1u)
           << static_cast<ComponentMask>(static_cast<std::uint16_t>(id));
}

class Entity;  // 前向声明

/// 组件抽象基类（§7）。具体组件继承并实现 Type()（推荐用下方 CRTP Component<>
/// 基类，自动获得正确的 Type() 实现）。OnAttached / OnDetached 在挂载 / 卸载时由
/// EntityManager 调用（§15.5 / §15.6）。
class IComponent {
public:
    virtual ~IComponent() = default;

    /// 返回该组件类型的 ComponentTypeId（用于类型安全查找与掩码）。
    virtual ComponentTypeId Type() const noexcept = 0;

    /// 挂载到实体时调用（如缓存反向引用）。
    virtual void OnAttached(Entity&) {}

    /// 从实体卸载时调用（如清理跨组件引用）。
    virtual void OnDetached(Entity&) {}
};

/// CRTP 便捷基类：派生类 `class Foo : public Component<Foo>` 即自动获得正确的
/// Type() 实现，无需手动维护 ID 注册表（§15.3）。
template <typename Derived>
class Component : public IComponent {
public:
    ComponentTypeId Type() const noexcept override;
};

/// 组件 ID 分配器（定义见 component_store.h；此处仅前向声明供 Component<>::Type 使用）。
template <typename C>
ComponentTypeId ComponentTypeIdFor() noexcept;

template <typename Derived>
ComponentTypeId Component<Derived>::Type() const noexcept {
    return ComponentTypeIdFor<Derived>();
}

class EntityManager;  // 前向声明：Entity 通过 mgr_ 转调组件操作

/// 实体（§7）。固定大小、可池化；组件数据不内联（存于稀疏数组）。
class Entity {
public:
    EntityId Id() const noexcept { return id_; }
    EntityType Type() const noexcept { return type_; }
    SceneId Scene() const noexcept { return scene_; }
    const Position& Pos() const noexcept { return pos_; }
    void SetPos(const Position& p) noexcept { pos_ = p; }

    bool Alive() const noexcept { return alive_; }

    /// 版本号 = 槽位世代号（销毁时 +1），供读快照做乐观并发校验（§7）。
    std::uint32_t Version() const noexcept { return EntityGeneration(id_); }

    // —— 组件操作（模板，依赖 EntityManager，定义于 entity_manager.h）——
    template <typename C, typename... Args>
    C* AddComponent(Args&&... args);

    template <typename C>
    C* TryGet() noexcept;

    template <typename C>
    const C* TryGet() const noexcept;

    template <typename C>
    bool RemoveComponent() noexcept;

    // —— 以下仅由 EntityManager 在生命周期内读写（友元访问）——
    EntityManager* Owner() const noexcept { return mgr_; }
    void SetOwner(EntityManager* mgr) noexcept { mgr_ = mgr; }
    void BindIdentity(EntityId id, EntityType type, SceneId scene) noexcept {
        id_ = id;
        type_ = type;
        scene_ = scene;
    }
    void SetAlive(bool v) noexcept { alive_ = v; }
    ComponentMask& Components() noexcept { return components_; }
    ComponentMask Components() const noexcept { return components_; }

private:
    EntityManager* mgr_ = nullptr;  // 弱引用，不拥有
    EntityId id_ = 0;
    SceneId scene_ = 0;
    Position pos_{};
    EntityType type_ = EntityType::Player;
    bool alive_ = false;
    // 编译器补齐至 8 字节对齐
    ComponentMask components_ = 0;  // 已挂载组件类型掩码

    friend class EntityManager;
};

static_assert(sizeof(Entity) <= 256,
              "Entity 必须保持 < 256B（§22 单实体内存预算，不含组件）");

}  // namespace mmo::game
