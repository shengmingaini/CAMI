# ARCHITECTURE.md

> 本文件配合 `PROJECT_REQUIREMENTS.md`（冻结规范）描述具体落地结构。架构冻结，改动须 RFC + 人工批准。

## 1. 四进程

| 进程 | 职责 | 禁止事项 |
|---|---|---|
| **Gateway** | 连接 / Session / 心跳 / 安全 / 限流 / 路由 | 不持有游戏实时状态；不做战斗逻辑 |
| **GameNode** | Scene / AOI / Movement / Combat / Role / Inventory / Quest / Social / Economy / Instance / Lua | 禁止直连 MySQL；战斗 Tick 禁止同步 Redis/gRPC/Kafka/DB |
| **DataService** | 统一 Redis / MySQL 访问 | 不实现游戏规则；仅 Load/Save/Update/Delete/Batch/VersionCheck |
| **ControlService** | 节点管理 / 配置下发 / 版本 / 热更 / 运维 | 不拥有任何游戏实时状态 |

## 2. GameNode 内部模块树（同进程，非独立进程）

```
GameNode
├── Core            (Error/Result, Logger/Trace, Time/UUID/Config, Memory/Thread/Scheduler, C/Q/E Bus)
├── Entity          (EntityID / Components / Create-Destroy-Find)
├── Scene           (Scene lifecycle, SceneID/Version/TickNumber/StateHash/Owner)
├── Scheduler       (20Hz 固定 Tick, 八阶段顺序执行)
├── AOI             (Dynamic Grid: Enter/Leave/Move/QueryVisible/Broadcast)
├── Movement        (Move/Direction/Speed/Position/Velocity, 防作弊校验)
├── Combat          (Skill/Damage/Heal/Buff/Threat/CombatFramework)
├── Role            (Player/Character 数据与行为状态)
├── Inventory       (Item/Equipment, 所有变更走 Command+Event)
├── Quest           (事件驱动, 禁止每秒遍历全量玩家)
├── Social          (Party/Friend/Guild/Chat/Mail)
├── Economy         (Currency/Reward/Trade/Auction, EconomyCommand + Ledger)
├── Instance        (World/Dungeon/Arena, 第一版不拆服务)
├── Lua             (Runtime / HotReload / Gameplay Script)
└── PersistenceAdapter (异步落库适配, 不阻塞 Tick)
```

依赖方向单向：**Game → Gameplay → Core**。禁止循环依赖。

## 3. 通信模型（C-Q-E）

| 类型 | 语义 | 用途 |
|---|---|---|
| Command | 执行操作 | CastSkill / MovePlayer / BuyItem |
| Query | 只读查询 | GetPlayer / GetInventory |
| Event | 事实已发生 | PlayerMoved / DamageApplied |

- 同进程：C++ Interface + Command + Event
- 跨进程：gRPC + Protobuf（仅低频 / 管理面 / 数据面；**禁止战斗 Tick 内同步 RPC**）
- 异步业务：EventBus / Kafka

统一 Envelope：`MessageID/MessageType/Version/Source/Timestamp/TraceID/RequestID/Payload`；
经济操作额外 `TransactionID/IdempotencyKey`。

## 4. 状态 Ownership 表

| 状态 | 权威 Owner | 禁止 |
|---|---|---|
| Position / HP / MP / Buff / CombatState | 当前 Scene / Instance | Scene 与 PlayerService 同时作 Owner |
| Session / Routing / Ranking | Redis（临时） | 作为实时状态唯一恢复源 |
| Character / Quest / Inventory / Equipment | MySQL（异步落库） | GameNode 同步直写 |
| Currency / Trade / Auction | Economy + Ledger（强一致） | 无 IdempotencyKey 重试 |

## 5. Tick 阶段（20Hz / 50ms）

```
Input → Movement → AOI → Combat → Buff → Quest → Event → Replication
```

每阶段独立计时统计；实时状态单 Owner + 顺序执行；非热路径允许局部锁，禁止全局锁。

## 6. 客户端（独立于服务器业务）

Core / Network / World / Entity / Rendering / UI / Resource / Audio。
协议直接复用 `protocol/` 定义（Protobuf + FlatBuffers），不另起一套。
Low / Medium / High 三档；Chunk Streaming + LOD + 资源动态释放；地图不常驻内存。
