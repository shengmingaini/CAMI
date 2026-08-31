# TASK-014 · AOI System — Interface Contract

> 模块：`server/gamenode/aoi`（GameNode 子树第四个模块，前接 entity/scene/scheduler）
> 命名空间：`mmo::game::aoi`
> 职责（§4 State Owner）：独占 AOI 索引（格子 → 实体集合）的写入权；可见集由 AOI 实时计算，其他模块只读。

## 1. 公开接口（冻结契约，见 TASK-014 §7）

```cpp
struct AoiConfig { float cell_size{20.0f}; float view_radius{50.0f};
                   size_t max_entities{10000}; bool use_dynamic_grid{true}; };
class IAoi {
  virtual core::Result<void> Enter(EntityId, const Position&) = 0;       // 入格 + 建立初始可见关系
  virtual core::Result<void> Leave(EntityId) = 0;                        // 移除 + 解除可见关系
  virtual core::Result<MoveResult> Move(EntityId, const Position& to) = 0; // 增量 diff（entered/left）
  virtual core::Result<void> QueryVisible(EntityId, std::vector<EntityId>& out) const = 0;
  virtual core::Result<size_t> Broadcast(EntityId, std::span<const uint8_t> payload) = 0; // 只发可见者，返回目标数
  virtual AoiStats Stats() const noexcept = 0;
};
struct MoveResult { std::vector<EntityId> entered; std::vector<EntityId> left; uint32_t touched_cells{0}; };
struct AoiStats { size_t entity_count, cell_count, avg_visible; uint64_t last_query_ns, last_broadcast_ns; size_t query_count; };
std::unique_ptr<IAoi> CreateDynamicGridAoi(AoiConfig = {});
```

- 类型：`EntityId` / `Position` 复用 TASK-011 实体模块（`mmo::game::EntityId` / `mmo::game::Position`），
  本头以 `using EntityId = mmo::game::EntityId; using Position = mmo::game::Position;` 对齐任务书 `entity::` 简写
  （禁止重复定义第二套实体类型，见 TASK-012 §27.2）。
- 工厂签名冻结（`CreateDynamicGridAoi(AoiConfig)`），无外部依赖；EventBus/传输由扩展点另行注入（见下）。

## 2. 算法（§8 / §15，Dynamic Grid）

- **稀疏格子哈希**：`unordered_map<CellKey, vector<EntityId>>`，世界坐标 `CellOf(pos)=floor(pos/cell_size)`。
  不用全覆盖二维数组（避免内存爆炸，§21）。CellKey 用 32-bit 交错哈希减少聚集。
- **查询局部化**：`ComputeVisible` 只扫描 `ceil(view_radius/cell_size)` 邻域格子（默认 50/20→R=3，即 7×7），
  再 3D 距离过滤（≤ view_radius），天然去重（每实体仅在一格）。**禁止全 Scene O(N) 扫描**（§21 / §20 验收 #4）。
- **可见集不缓存**：每次 `QueryVisible` / `Move` / `Broadcast` 由位置实时重算，保证与暴力 O(N²) 参考实现
  **100% 一致**（§20 验收 #2），且单实体 AOI 内存 < 128B（§22）。
- **增量 diff**：`Move` 重算移动前后可见集，排序后 `std::set_difference` 得 entered/left（O(k)，禁止全量重算，§15.6）。
- **跨格增量**：仅从旧格移除、插入新格（O(1)），不重建全表。

## 3. 异常 / 边界（§19 / §21）

| 情形 | 行为 |
|------|------|
| 重复 Enter 同 id | INVALID_ARGUMENT |
| 未知 id 的 Leave/Move/Query/Broadcast | NOT_FOUND |
| NaN 坐标（Enter/Move） | INVALID_ARGUMENT（拒绝入索引，不产生越界访问） |
| 越界坐标（\|coord\| > 1e6 m） | INVALID_ARGUMENT |
| 实体数 ≥ max_entities | BUSY（§19 容量保护） |
| 空格查询 | 返回空集不崩（Stats 计数 0） |
| 瞬移跨多格 | 正确产生一次大 diff（重算即正确，非漏通知） |

## 4. 扩展点（非冻结接口，供集成测试 / 下游注入）

`DynamicGridAoi`（具体类，不在 `IAoi` 内）提供：
- `SetBroadcastSink(BroadcastSink)`：广播投递回调 `(from, to, payload)`，供网络层消费（§17 可观测投递）。
- `SetNotifySink(NotifySink)`：视野变化通知 `(watcher, seen, entered)`，对应 §8 的 EntityEnteredView / EntityLeftView 事件。

## 5. 内存追踪（§22 / §24）

`dynamic_grid_aoi.h` 定义 `TrackingAlloc`（全局 `g_aoi_heap_bytes` 计数器），被 AOI 的 `unordered_map` /
`vector` 容器使用；`ResetAoiHeapTracking()` / `AoiHeapBytes()` 供 benchmark 测 `mem_bytes_per_entity`。
`Broadcast` 复用单一成员缓冲区 `send_buf_`，每次只 resize 一次，禁止每目标单独分配/序列化（§15.5）。

## 6. 依赖方向（§27.3）

```
server/gamenode/aoi -> engine/core（error / time）
                 -> server/gamenode/entity（EntityId / Position，§27.2 消费公开接口）
```
- 红线：公开头（include/）禁止 include 本模块 src/；禁止全局锁；禁止在 STATUS:DONE 后静默改接口签名。
