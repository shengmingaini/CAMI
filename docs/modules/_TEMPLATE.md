# [模块名] 模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: [接入层 / 逻辑业务层 / 数据管理层 / 全局公共服务层 / 运维控制层]  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/*.fbs`  
> **关联 ADR**: [列出本模块直接相关的 ADR 编号]

---

## 1. 模块概述

- **定位**：一句话说明模块在架构中的位置。
- **核心职责**：
  1. 职责一
  2. 职责二
- **不在职责内（边界）**：明确写出"不做什么"，防止 scope creep（如战斗模块不直接写角色字段）。

## 2. 架构约束与边界

> 引用 WoW 模块边界（架构 §4.2.2 / ADR-002）。

- **是否拥有 `Player` 对象**：是 / 否
- **读写契约**（仅当模块不直接拥有 Player 时）：
  - 读：`const Player&` 引用直接内存访问，零开销
  - 写：仅经由 `Player::ApplyXxx()` 方法，由角色模块内部校验
- **红线禁令**：禁止战斗循环内写 DB、禁止 JSON 高频、禁止无 AOI 全服广播（架构 §1.3）。

## 3. 对外接口（C++ 签名级）

命名规范（架构 §8.1）：类 `PascalCase`，方法 `camelCase`，变量 `snake_case`，命名空间 `cami::xxx`。

### 3.1 查询接口（返回 const 引用）
```cpp
const CharacterStats& stats() const;
```

### 3.2 修改接口（含校验）
```cpp
int32_t applyDamage(uint64_t source_id, int32_t raw_damage, DamageType type);
```

### 3.3 生命周期接口
```cpp
void onEnterScene(uint32_t scene_id, const Vec3& spawn, float yaw);
void onLeaveScene();
void persist();  // WAL flush
```

## 4. 核心数据结构

- **内存布局**：OOP 对象 / ECS SoA 布局（架构 v3.0，高频属性顺序访问）。
- **关键字段**：列出状态字段、版本号（乐观锁，`m_version`）、dirty 标记。
- **对象池**：实体（怪物/Buff/技能）全对象池预分配（架构 §10.3）。

## 5. 事件契约（进程内 EventBus）

> GameNode 内模块间一律走 EventBus（架构 §3.3 / ADR-001），禁止直接跨模块访问内部成员。

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `PlayerStatChangedEvent` | `{player_id, mask, delta}` | AOI、Quest、UI |

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理逻辑 |
|------|--------|---------|
| `SkillCastIntentEvent` | 连接处理 | 执行施法 |

### 5.3 事件结构定义（示例）
```cpp
struct PlayerStatChangedEvent {
    uint64_t player_id;
    uint32_t changed_mask;   // 标记哪些属性变化
    // 不携带完整快照，AOI 按需向角色模块查询
};
```

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody` 枚举） | 来源 schema |
|------|-----------------------------------|-------------|
| C→S | `SkillCastIntent` | `proto/flatbuffers/combat.fbs` |
| S→C | `DamageEvent` / `BuffEvent` / `CombatResult` | `proto/flatbuffers/combat.fbs` |

- 高频逐帧移动/朝向走**轨道 A 位压缩**（protocol-spec §7），不携 FlatBuffers 表。
- 可靠消息统一封入 `MessageEnvelope`（envelope.fbs），类型由 `body_type` 判别。

## 7. 并发模型

- **线程归属**：场景级线程隔离（ADR-012），本模块运行于所属场景的线程。
- **锁策略**：同线程内访问无锁；跨场景通过 MPMC 无锁队列（架构 §4.2）。
- **Job System**：非关键任务（邮件/日志/排行榜）提交 Job System，不阻塞场景主循环。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | — |
| 单节点消息 | ≤20,000/s | — |
| 跨节点 AOI | ≤10/s | — |

## 9. 依赖方向

```
[调用方] ──→ [本模块] ──→ [被调用方]
```
- **上游（调用本模块）**：列出
- **下游（本模块调用）**：列出，禁止逆向回调

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式 | ADR-002 |
| 场景级线程隔离 | ADR-012 |

## 11. 开放问题 / 后续

- 待补充：配置表 Protobuf schema（物品/技能）。
- 待补充：跨服事件具体实现（架构 §13 待解决问题 #1）。
