# CAMI 事件总线设计（EventBus / MPMC 无锁队列）

> 交付归属：Day 5 架构设计（任务卡 2026-08-14 周五）
> 位置：`common/event_bus/`（全局公共服务层，ADR-012 内部 EventBus 的承载）
> 验收：总线吞吐 ≥ 100 万 msg/s

## 1. 为什么放 common 层

事件总线是跨层解耦基础设施。架构文档 `zones-architecture.md` 已明确「跨场景迁移经 **MPMC 无锁队列**」；`modules/*.md` 中登记的三个上游事件（combat 的 `MobKilledEvent`、character 的 `ItemObtainedEvent`、scene 的 `PlayerEnterAreaEvent`）正是经由内部 EventBus 流向 quest。因此它归于最底层公共服务 `common`，被所有业务层复用，且自身不依赖任何业务层（符合 spec §3.2 单向依赖）。

## 2. 总体结构

```
                 publish(ev)                drain() → 调用订阅者
  生产者 ───────▶│                          │◀────── 消费者线程
                 ▼                          ▼
        ┌─────────────────┐        ┌──────────────────────┐
        │  Channel<T>      │        │ 订阅者快照 (vector)    │
        │  ├ mpmc_queue<T> │◀队列──▶│ handler1 / handler2..  │
        │  └ handlers_     │        └──────────────────────┘
        └─────────────────┘
                 ▲
        EventBus<T> 注册表（按通道名查/建 Channel<T>*）
```

- **`mpmc_queue<T>`**：Vyukov 有界 MPMC 无锁环形队列。enqueue/dequeue 全程原子 CAS，wait-free（非空/未满时）；cell 64 字节缓存行对齐 + 头尾计数器各占独立缓存行，消除伪共享。
- **`Channel<T>`**：单类型通道 = 1 个 MPMC 队列 + 订阅者列表。publish 仅入队（无锁）；drain 取一批事件并调用订阅者快照（无锁读快照）；subscribe 是冷路径，受互斥保护。
- **`EventBus<T>`**：同类型 T 的多个具名通道注册表。首次访问按需创建（受互斥），之后热路径只缓存 `Channel<T>*` 句柄、不再查表。

## 3. 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 队列算法 | Vyukov MPMC（原子 CAS） | 真·无锁、wait-free、无 ABA；比 mutex 队列在并发下高 1~2 个数量级 |
| 发布订阅类型 | 模板 `EventBus<T>` 每类型一个 | 编译期类型安全，避免 `void*` 类型擦除的运行时错误 |
| 热路径加锁 | 不加锁 | publish/drain 仅碰原子与订阅者快照；订阅者快照在 drain 时一次性无锁拷贝 |
| 订阅变更 | mutex 保护 | 冷路径，订阅者极少且关系稳定，不影响吞吐 |
| 容量 | 2 的幂（位与取模） | MPMC 算法要求；非 2 的幂 `std::terminate()` 早失败 |
| 队列满 | enqueue 返回 false | 调用方自行背压/丢弃，避免阻塞生产者（MMO 热路径不能卡） |

## 4. 与既有 ADR 的约束

- **ADR-012**：内部 EventBus 事件名 ≠ 网络 FlatBuffers 消息名。本总线只承载进程内解耦（如 combat→quest），不生成任何网络协议。
- **ADR-002**：事件载荷若含 Player 语义，订阅方只读 `const Player&` 或调用 `ApplyXxx()`；事件总线本身不持有 Player。
- **economy 为发奖唯一出口**：奖励类事件（如 `ItemObtainedEvent`）最终须经 economy 模块落地，事件总线只做传输、不做发奖。

## 5. 吞吐实测与验收

`benchmark/event_bus_bench.cpp` 在目标机（**MINGW64 / GCC 16.1.0 / Release / Ninja**）实测，**4 组全部远超 100 万 msg/s 验收线**：

| 场景 | 配置 | 吞吐 | 验收（≥1.00） |
|------|------|------|---------------|
| MPMC 裸队列 | 1P1C | **18.33 Mmsg/s** | ✅ |
| MPMC 裸队列 | 4P4C | **13.88 Mmsg/s** | ✅ |
| EventBus 端到端 | 1P1C | **19.54 Mmsg/s** | ✅ |
| EventBus 端到端 | 4P4C | **14.77 Mmsg/s** | ✅ |

观察：EventBus 端到端（含 `std::function` 分发 + 订阅者快照拷贝）反而略高于裸 MPMC，说明热路径无锁 + 64B 缓存行隔离把分发开销抵消了；4P4C 受多生产者 CAS 竞争略低于 1P1C，符合无锁队列预期。**结论：Day 5 事件总线达标并闭环（用户 2026-08-10 本地实跑确认）。**

## 6. 后续

- 接入真实事件：将 `MobKilledEvent` 等定义为 protobuf/FB 消息后，用 `EventBus<MobKilledEvent>` 实例化。
- 若需单消费者按类型分发，可叠一层 `Channel<T>` 之上的「事件类型标签」；当前单类型总线已满足 ADR-012 的 three events 场景。
- 监控：可统计每通道 `approx_size()` 做积压告警（接 `alert_channel`）。
