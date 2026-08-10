# AOI 模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.4)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/aoi.fbs` + `movement.fbs` + `common.fbs`  
> **关联 ADR**: ADR-007（LOD 三级）、ADR-012（场景线程）、ADR-013（L1 缓存）、v3.0 差分/预测/跨节点聚合

---

## 1. 模块概述

- **定位**：兴趣域管理。维护「谁对谁可见」的关系图，按距离分级（LOD）产出增量同步，是所有玩家可见性消息的唯一生产者。
- **核心职责**：
  1. 维护每观察者的实体集合（十字链表 + 动态网格 + 层次网格 10m/50m/200m）。
  2. LOD 三级更新频率（LOD0 0–30m 每帧 / LOD1 30–80m 100ms / LOD2 80–200m 500ms）。
  3. 视野锥裁剪（270°）、动态负载均衡（拥挤区缩 LOD0 半径）。
  4. 属性压缩 + **差分压缩**（同实体仅首发全量，后续仅发 delta，消息量降 60–80%）。
  5. 预测预取（按移动方向/速度预加载 3s 内将入视野的实体属性）。
  6. 跨节点 AOI 聚合（边界实体每 5s 批量同步，消息量 ≤10 条/s）。
- **不在职责内**：不耦合战斗/移动逻辑（架构 §4.2.4）；不持有玩家数据（属性经角色模块查询）；不做网络收发（产出消息由 Gateway 分发）。

## 2. 架构约束与边界（WoW 模式）

- **拥有 `Player` 对象**：否。
- **读契约**：需属性时通过 `const PlayerCharacter&` 向 character 模块查询（如生成 `AoiEnter.attr` 首包全量快照）。
- **写契约**：无 —— AOI 永不修改角色数据，仅读取 + 广播。
- **强制**：所有可见性消息**必须经 AOI 作用域过滤**，禁止无 AOI 全服广播（红线 §1.3）。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::aoi`。类 `AOIManager`（每场景一个实例，运行于场景线程）。

### 3.1 可见性维护
```cpp
void onEntityEnter(uint64_t entity_id, EntityKind kind);   // 实体进入场景
void onEntityLeave(uint64_t entity_id);                    // 实体离开场景
void onEntityMove(uint64_t entity_id, const Vec3& pos, float yaw, MoveState st); // 移动采样
void rebuildObserverSets();                                 // 每 tick 重算观察者集合
```

### 3.2 增量产出（驱动网络消息）
```cpp
// 生成 AoiEnter（首发全量）：向 observer 推送 entity 的 MovementSnapshot + AttributeSnapshot
void emitEnter(uint64_t observer_id, uint64_t entity_id);
// 生成 AoiUpdate（差分）：按 observer→entity 的 LOD 决定属性精度
void emitUpdate(uint64_t observer_id, uint64_t entity_id, uint32_t changed_mask);
// 生成 AoiLeave
void emitLeave(uint64_t observer_id, uint64_t entity_id);
// 服务器纠偏（预测误差 >0.5m，架构 §4.2.11）
void emitCorrection(uint64_t observer_id, uint64_t entity_id);
```

### 3.3 跨节点聚合
```cpp
void flushCrossNodeAggregate();   // 每 5s：合并本节点边界实体状态，单条消息跨节点同步
```

## 4. 核心数据结构

- **空间索引**：层次网格（近 10m / 中 50m / 远 200m）+ 十字链表（双向邻接）。实体 ID 仅存引用，不存属性。
- **观察者集合**：`ObserverEntry { entity_id, lod_level, last_send_ts, last_full_snapshot }`。
- **差分状态**：每个 (observer, entity) 对维护上一次下发的 `AttributeSnapshot` 摘要，计算 delta（变动字段）决定是否下发 `AoiUpdate.attr`。
- **预测预取缓存**：基于速度向量的短期预判表（3s 窗口），命中即提前向 character 拉取属性。
- **对象池**：`ObserverEntry` 等全池化（架构 §10.3）。

## 5. 事件契约（进程内 EventBus）

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| （AOI 直接产出网络消息，不经 EventBus 中转，见 §6） | — | Gateway（经 SHM/IPC 分发） |
| `CrossNodeAoiSyncEvent` | `{scene_id, border_entities_delta}` | 跨节点聚合器（gRPC/Redis Pub/Sub） |

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理 |
|------|--------|------|
| `PlayerEnteredSceneEvent` | character | `emitEnter` 给同场景相关观察者 |
| `PlayerLeftSceneEvent` | character | `emitLeave` |
| `PlayerStatChangedEvent` | character | 触发相关观察者 `emitUpdate`（按 changed_mask + LOD 裁剪） |
| `DamageDealtEvent` / `CombatResultEvent` | combat | 触发 `emitUpdate`（hp/死亡变化→构造网络 `DamageEvent`/`CombatResult` 广播） |
| `BuffAppliedEvent` / `BuffRemovedEvent` | character（由 combat 调 `applyBuff` 触发） | 触发 `emitUpdate`（构造网络 `BuffEvent` 广播） |
| `EntityMovedEvent` | scene（移动采样） | `onEntityMove` → 重算 LOD / 视野锥 |

### 5.3 事件结构（沿用架构 §4.2 风格）
```cpp
struct EntityMovedEvent {
    uint64_t entity_id;
    Vec3     pos;
    float    yaw;
    MoveState st;
};
```

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody`） | schema |
|------|-------------------------------|--------|
| S→C | `AoiEnter` | `aoi.fbs` |
| S→C | `AoiLeave` | `aoi.fbs` |
| S→C | `AoiUpdate` | `aoi.fbs` |
| S→C | `MovementSnapshot` | `movement.fbs`（首发全量 / 纠偏） |
| S→C | `Correction` | `movement.fbs`（预测纠偏） |

- `AoiEnter.attr` 填 `AttributeSnapshot`（common.fbs）完整属性；`AoiUpdate.attr` 仅 LOD0 填充，LOD1/2 为空（差异化由 `changed_mask` + LOD 决定）。
- 高频逐帧移动/朝向走**轨道 A 位压缩**（protocol-spec §7），由本模块编码后走 UDP；`AoiUpdate.move` 全量快照仅用于首发/纠偏/误差回滚。
- 所有可靠消息封入 `MessageEnvelope`（`envelope.fbs`）。

## 7. 并发模型

- **线程归属**：每场景一个 `AOIManager`，运行于**场景线程**（ADR-012）。与 character / combat 同线程，查询 `const Player&` 无锁。
- **跨节点同步**：`flushCrossNodeAggregate` 由场景 tick 每 5s 调用，产出单条聚合消息经 gRPC / Redis Pub/Sub 发往相邻节点（架构 §3.3），消息量 ≤10 条/s。
- **Job System**：预测预取的属性批量拉取可提交 Job System，不阻塞主循环。
- **跨场景**：经 MPMC 无锁队列（架构 §4.2）。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 跨节点 AOI 消息量 | ≤10 条/s | 边界实体 5s 聚合单条同步（v3.0 聚合） |
| 高频带宽(1000人) | ≤50 KB/s | LOD 分级 + 差分压缩 + 属性压缩 + 视野锥裁剪（ADR-007 / v3.0） |
| 单节点消息 | ≤20,000/s | 仅广播给 AOI 作用域，禁止全服 |
| LOD0 更新 | 每帧(33ms) | 仅 0–30m 实体，属性 ~47B |
| LOD1 / LOD2 | 100ms / 500ms | 关键/最小属性（~10B / ~5B） |
| 预测正确率 | ≥95% | 配合客户端预测 + 纠偏（ADR-005） |

## 9. 依赖方向

```
[character] ──PlayerStatChanged/Enter/Leave──→ [aoi] ──AoiEnter/Update/Leave──→ [Gateway(SHM)]
[combat] ──DamageEvent/BuffEvent──→ [aoi]
[scene] ──EntityMoved──→ [aoi]
[aoi] ──查询 const Player&──→ [character]
[aoi] ──cross-node sync──→ [跨节点聚合器(gRPC/Redis Pub/Sub)]
```
- **上游**：character、combat、scene（事件源）。
- **下游**：character（属性查询）、Gateway（IPC 分发）、跨节点聚合器。
- **禁止逆向**：character / combat 不得回调 AOI 内部集合。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| AOI LOD 三级 + 属性压缩 + 视野锥 | ADR-007 |
| 场景级线程隔离 | ADR-012 |
| L1 缓存（属性查询提速） | ADR-013 |
| 差分压缩 / 预测预取 / 跨节点聚合 | 架构 §4.2.4（v3.0） |
| 客户端预测 + 纠偏 | ADR-005（AOI 下发 Correction） |

## 11. 开放问题 / 后续

- Cell 边界 AOI 平滑迁移策略（架构 §13 #8，P2）。
- 跨节点聚合器的具体 gRPC/Redis Pub/Sub 实现（架构 §13 #1，P1）。
- 视野锥朝向数据来源：依赖 movement 模块的 `yaw` 采样频率，需与移动同步节拍对齐。
