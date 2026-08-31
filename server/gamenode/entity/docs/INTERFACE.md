# Entity System · INTERFACE（TASK-011）

> 冻结后不可破坏性变更；下游依赖本文件导出的公开接口。

## 命名空间
`mmo::game`

## 类型
| 类型 | 说明 |
|---|---|
| `EntityId` (`uint64_t`) | `(index<<32)\|generation`，全局唯一、销毁后不可复用 |
| `SceneId` (`uint64_t`) | 所属场景 ID；`kInvalidSceneId = 0` |
| `EntityType` (`uint8_t`) | `Player/Monster/Npc/Projectile/Effect/Item` |
| `Position` | `{float x,y,z,yaw}` |
| `ComponentTypeId` (`uint16_t`) | 组件类型强类型 ID，由 `ComponentTypeIdFor<C>()` 分配 |
| `ComponentMask` (`uint64_t`) | 实体已挂载组件类型掩码（≤64 种） |

## 组件抽象
```cpp
class IComponent {
  virtual ~IComponent() = default;
  virtual ComponentTypeId Type() const noexcept = 0;     // 类型安全查找键
  virtual void OnAttached(Entity&) {}
  virtual void OnDetached(Entity&) {}
};
// 推荐：class Foo : public Component<Foo> { ... };  自动获得 Type()
```

## Entity
```cpp
EntityId    Id() const noexcept;
EntityType  Type() const noexcept;
SceneId     Scene() const noexcept;
const Position& Pos() const noexcept;
void        SetPos(const Position&) noexcept;
bool        Alive() const noexcept;
uint32_t    Version() const noexcept;          // = 世代号，供读快照校验

template <typename C, typename... Args> C* AddComponent(Args&&...);
template <typename C> C* TryGet() noexcept;
template <typename C> const C* TryGet() const noexcept;
template <typename C> bool RemoveComponent() noexcept;   // 未挂载返回 false
```

## EntityManager
```cpp
explicit EntityManager(core::EventBus* bus = nullptr, size_t max_entities = 4'000'000);
core::Result<Entity*> Create(EntityType, SceneId, const Position&);
core::Result<void>    Destroy(EntityId);     // 延迟；重复 Destroy → NOT_FOUND
Entity* Find(EntityId) noexcept;              // O(1)，防 ABA
template <typename C> void Each(std::function<void(Entity&, C&)>);
size_t Count(EntityType) const noexcept;
size_t AliveCount() const noexcept;
void FlushDeferred() noexcept;                // Tick 边界物理回收
static constexpr size_t MemoryBytesPerEntity() noexcept;  // 64B（不含组件）
```

## 生命周期事件（EventBus）
`EntityCreated{id,type,scene}` / `EntityDestroyed{id,type,scene}` / `ComponentAttached{id,type}` / `ComponentDetached{id,type}`。均为 POD、nothrow、≤32B，满足 EventBus 内联预算。

## 错误码
`BUSY`（超上限）/ `NOT_FOUND`（不存在或已销毁）。
