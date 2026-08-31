# Entity System（TASK-011 · Phase 3 GameNode Core）

统一实体系统：EntityID / EntityType / SceneID / Position / Components，支持 Create / Destroy / Find / AttachComponent / RemoveComponent。第一版采用 ECS-like 组件思想，不强制完整 ECS 框架。

## 模块位置
`server/gamenode/entity`

- `include/mmo/game/entity/`：`entity_id.h` `entity.h` `component_store.h` `entity_events.h` `entity_manager.h`（全部公开接口，禁止下游 `#include` 本模块 `src/`）
- `src/entity_manager.cpp`：非模板实现（Create / Destroy / Find / FlushDeferred / DetachAll）
- `tests/`：`entity_test.cpp`（单元/集成/Failure）、`entity_bench.cpp`（性能）
- `docs/`：五文档契约

## 关键设计
1. **SlotMap + generation（防 ABA）**：`EntityId = (index<<32)|generation`。销毁时世代 +1，旧 id 查找必返回 nullptr（`entity_manager.h::Find`）。
2. **延迟销毁**：`Destroy` 仅逻辑死亡 + 世代 +1；物理回收（释放槽位 + 组件 + 发布 `EntityDestroyed`）推迟到 `FlushDeferred`（Tick 边界），避免 Tick 中途悬垂。
3. **组件稀疏数组**：每组件类型一个 `ComponentStore<C>`（EnTT 风格 dense/sparse），遍历局部性好、增删 O(1)。
4. **生命周期事件**：`EntityCreated / EntityDestroyed / ComponentAttached / ComponentDetached` 走 `EventBus`（异步，宿主线程 `Drain` 派发）。
5. **池化**：实体对象来自 `ObjectPool<Entity>`（TASK-004），热路径无分配。

## 构建 / 验证
```bash
# 本地验收（本仓库 build 缓存为 Ninja，须注入 GENERATOR=Ninja）
GENERATOR=Ninja bash scripts/verify/task-011.sh
bash scripts/task-done.sh TASK-011
```
