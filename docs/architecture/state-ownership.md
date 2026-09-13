# 状态归属（TASK-038 交付 · docs/architecture/state-ownership.md）

> 对应 PROJECT_REQUIREMENTS §10 / §12：同一实时状态只能有一个权威 Owner；跨模块写入必须走 Command。

## 1. 权威所有权矩阵

| 状态 | 权威 Owner | 存储位置 | 禁止的写入者 |
|---|---|---|---|
| Position / HP / MP / Buff / CombatState / MovementState | 当前 Scene / Instance（GameNode 内） | GameNode 内存 | Scene 之外的任何进程/模块直接写 |
| Session | Gateway + Redis（缓存/路由） | Gateway 内存 + Redis | GameNode 直写 Redis 作为权威 |
| Character / Quest / Inventory / Equipment | DataService（异步持久化） | MySQL | GameNode 同步直写 MySQL |
| Currency / Trade / Auction / Purchase | Economy Ledger（append-only + 幂等） | MySQL + Redis 缓存 | 无幂等键的重复写入 |
| Routing（Player→GameNode→Scene） | ControlService / Gateway | Redis | GameNode 自管全局路由表 |
| Bot 自身状态 | 各 Bot 实例（无共享） | Bot 进程内存 | 压测采集器进程 |

## 2. Bot / 压测状态归属（TASK-038 §4）

- 每个 Bot 实例独立持有自身 `BotStats`（actions_done / errors / rtt / received_pps），**无共享写者**。
- 压测汇总指标由独立的采集器进程/脚本拉取并聚合，不反向写入被测进程（§9 红线：禁止在被测机器上跑 Bot 污染数据）。
- `BotFarm` 只聚合只读统计，不修改任何被测服务状态。

## 3. 跨模块写入规约

- 一律通过公开 Interface / Command / Event 通信（§27.3）；禁止访问依赖模块内部成员（`otherModule.internalData`）。
- 经济操作必须带 `TransactionID + IdempotencyKey`（§16），保证重试不重复扣款/发奖、不产生复制装备。
- 关键状态写入必须带 `Version`，无 Version 的写入禁止（§33 红线）。

## 4. 故障态归属（TASK-037 / TASK-038 §38.4）

- GameNode 故障：Detect → Freeze → Locate Replacement → Reattach → Resume（玩家可重连优先）。
- Chaos 演练中状态归属不变；恢复断言只校验「状态未丢失 / 可重连 / RTO 在预期内」，不校验「是否发生抖动」。
