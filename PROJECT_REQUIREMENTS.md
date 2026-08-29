# Project Requirements V1.0 — Frozen Architecture

> **本文件冻结。任何修改必须提交 Architecture Change Request（RFC）并经人工批准。**
> 默认冻结，AI / 开发成员不得自行改写。如发现架构级缺陷，先提 RFC，批准后再改。
> 正常开发过程中默认本规范冻结，不允许自行修改。

---

## 1. 项目目标

构建模块化、低资源、可水平扩展的 MMORPG 后端与客户端。

- 服务器架构支持 **50,000 CCU**
- GameNode 无单点
- 实时状态单一 Owner
- 战斗/移动热路径无阻塞 IO
- 禁止 O(N) 全服广播
- 支持玩家重连、节点故障恢复和 Scene 迁移
- 支持 Lua 玩法热更新
- 客户端优先兼容老旧 PC
- 所有模块可独立开发、测试和验收，最终组合运行

## 2. 核心原则

**少进程、强模块、统一接口、统一通信、单一状态 Owner、热路径零阻塞、可测试、可扩展。**

1. **Module ≠ Process**
2. 同一实时状态只能有一个权威 Owner
3. 高频热路径禁止远程 RPC 和外部 IO
4. 禁止全局锁作为核心并发模型
5. 禁止 O(N) 全服广播
6. 禁止 GameNode 直接访问 MySQL
7. 所有跨服务 Command 必须支持版本和幂等
8. 所有性能指标必须通过 Benchmark 验证

## 3. 进程架构（四类核心进程）

```
Gateway
GameNode
DataService
ControlService
```

- **Gateway**：网络连接、Session、心跳、安全、限流、路由。
- **GameNode**：实时游戏逻辑（Scene/Entity/Movement/AOI/Combat/Role/Inventory/Quest/Social/Economy/Instance/Lua）。默认同进程模块，不强制拆进程。
- **DataService**：统一访问 Redis / MySQL；GameNode 禁止直连数据库。
- **ControlService**：Config / Node Management / Version / Hot Reload / Deployment。

## 4. 模块通信（三种消息）

- **Command**：执行操作（CastSkill / MovePlayer / BuyItem / EquipItem）
- **Query**：读取数据（GetPlayer / GetInventory / GetScene）
- **Event**：事件已发生（PlayerMoved / SkillCast / DamageApplied / QuestCompleted）

```
同进程：   C++ Interface + Command + Event
跨进程：   gRPC + Protobuf
异步业务： EventBus / Kafka
```

**禁止为了模块化而把同进程高频调用转换为网络 RPC。**

## 5. 统一消息 Envelope

所有 Command / Query / Event 使用统一 Envelope：

```
MessageID / MessageType / Version / Source / Timestamp / TraceID / RequestID / Payload
```

经济操作额外包含：`TransactionID / IdempotencyKey`。不得为不同模块创建第二套消息格式。

## 6. GameNode 状态模型

Scene 是实时状态的主要 Owner。在线玩家实时状态由当前 Scene/Instance 持有：

```
Position / HP / MP / Buff / CombatState / MovementState / TemporaryState
```

禁止 Scene 与 PlayerService 同时作为权威 Owner。Redis 不是实时状态的最终权威来源。

## 7. GameNode 模块

模块必须独立目录、接口、测试和文档，默认运行于同一 GameNode：

```
Core / Entity / Scene / Scheduler / Movement / AOI / Combat / Role /
Inventory / Quest / Social / Economy / Instance / Lua / PersistenceAdapter
```

模块之间：不得访问内部成员；不得形成循环依赖；必须通过公开 Interface / Command / Event 通信。

## 8. Tick 与并发模型

GameNode 固定 Tick，默认 **20Hz / 50ms**。Tick 顺序：

```
Input → Movement → AOI → Combat → Buff → Quest → Event → Replication
```

实时状态优先采用 **单 Owner + 顺序执行**，而非依赖大量锁。允许非热路径局部锁，禁止全局锁成为核心架构。

## 9. 热路径红线（Hot Path）

```
Movement / AOI / Combat / Buff / Skill / Position Update
```

Hot Path 禁止：MySQL、同步 Redis、同步 gRPC、同步 Kafka、文件 IO、阻塞网络 IO、高频不可控堆分配。

## 10. AOI

第一版 Dynamic Grid。统一接口：`Enter / Leave / Move / QueryVisible / Broadcast`。
所有视野计算必须局部化，禁止 O(N) 搜索。

## 11. Combat

负责：Skill / Damage / Heal / Buff / Threat / AOE / Interrupt / PVE / PVP。
Combat 不直接修改 Role 内部成员，使用 Role Interface / Command。战斗热路径不得访问外部服务。

## 12. Lua

用于：Quest / Skill Rules / Buff Formula / NPC AI / Boss Script / Dungeon Script / Event Script / Gameplay Rules。
C++ 负责：Simulation / Entity / Scheduler / Network / AOI / Memory / Core Combat Framework / Persistence。
**C++ 提供机制，Lua 提供规则。** 不得强制把全部玩法逻辑放入 Lua。

## 13. Lua 热更新

只能在 Tick Safe Point 切换：`Compile → Validate → Load → Safe Point → Activate → Rollback`。
不得在正在执行的 Tick 中途替换脚本。必须记录 `ScriptVersion / ConfigVersion`。

## 14. 数据系统（三类）

- 实时状态：保存在 GameNode（HP / MP / Position / Buff / CombatState）
- 普通持久化：异步保存（Character / Quest / Inventory / Equipment）
- 强一致经济数据：Currency / Trade / Auction / Purchase / Reward

## 15. DataService

统一接口：`Load / Save / Update / Delete / Batch / VersionCheck`。业务代码不得直接依赖 Redis/MySQL 实现。

## 16. Economy

所有经济操作经过统一 Economy Command：`AddCurrency / RemoveCurrency / AddItem / RemoveItem / Trade / Purchase / Reward`。
关键操作必须具备 `TransactionID / Version / IdempotencyKey`，保证重试不重复扣款/发奖/复制装备。

## 17. Redis / MySQL

- Redis：Session / Cache / Routing / Ranking / Temporary Data。不得将 RDB Snapshot 作为唯一故障恢复机制。
- MySQL：最终持久化。初始 8 logical shards，业务层不写死分片数。

## 18. Gateway / Session

Session 含：`SessionID / PlayerID / GatewayID / GameNodeID / SceneID / Version`。
GameNode 故障：`Detect → Freeze → Locate Replacement → Reattach → Resume`。优先保证可重连；Live Scene Migration 为后续能力。

## 19. World / Instance

统一由 GameNode 管理：World / Scene / Dungeon / Arena / Instance。第一版不拆独立服务。

## 20. 客户端

客户端必须独立于服务器业务代码，仅通过 Protocol 连接。核心模块：Core / Network / World / Entity / Rendering / UI / Resource / Audio。
目标：优先低 CPU / RAM / VRAM / 网络带宽，而非高画质。

## 21. 低配置客户端

必须支持 Low / Medium / High 三档。Low 模式重点限制：CPU / RAM / VRAM / Draw Call / Texture Memory / Particle Count / Entity Count / Network Update Rate。
必须支持地图 Chunk Streaming / LOD / Texture Cache / Mesh Cache / 资源动态加载释放。禁止整个大地图永久全部加载进内存。

## 22. 客户端同步

结合 AOI / Delta Update / Interest Management / Update Frequency / Compression。玩家主要接收自身及附近相关 Entity 数据。

## 23. 性能目标

1000 玩家 Scene 第一基准：Average Tick < 5ms，P95 < 5ms，P99 < 8ms。
必须拆分 Movement / AOI / Combat / Lua / Event / Serialization 测量，注明 **Benchmark Result**，不得把理论值描述成实测值。

## 24. 扩展目标

```
100 → 500 → 1,000 → 5,000 → 10,000 → 20,000 → 50,000 CCU
```

GameNode 初始 50 active + failover；Gateway 初始 10 active + failover。Redis/MySQL 分片数由压测确定。

## 25. 测试

每个模块必须提供：Unit / Integration / Benchmark / Failure Test。
核心系统额外：Replay / Determinism / Fuzz Test。
最终必须测试：Node Crash / Reconnect / Network Failure / Redis Failure / MySQL Failure。

## 26. Bot / 压测

独立 Bot Framework：Login / Move / Attack / Quest / Trade / Chat / Logout / Reconnect（无渲染）。
最终支持 1K / 5K / 10K / 20K / 50K CCU。

## 27. 代码规范

C++20 / CMake / clang-format / clang-tidy / GoogleTest。
重要类必须标注：Thread Ownership / Lifetime / State Ownership / Thread Safety / Hot Path / Cold Path。

## 28. AI / Codex 工作方式

必须严格：`Read Requirements → Inspect Code → Check Dependencies → Design → Implement → Compile → Unit Test → Integration Test → Benchmark → Commit`。
一个 Task 未通过验收，不得进入下一个 Task。

## 29. Task 原则

每个 Task 必须：独立目录 / 独立接口 / 独立测试 / 独立 Benchmark / 独立文档。
必须明确：Dependencies / Input / Output / State Owner / Public Interface / Thread Model / Hot Path / External IO / Tests / Benchmark / Acceptance Criteria / Forbidden。

## 30. 第一优先级

```
1. Correctness  2. Architecture Consistency  3. Maintainability
4. Testability  5. Performance  6. Scalability
```

禁止为局部性能优化破坏统一接口和模块边界。

## 31. 本地开发

开发/编译/测试/压测/网络模拟/故障注入尽可能本地完成。基础环境：Gateway / GameNode / DataService / Redis / MySQL；后续加 Docker / Local K8s / Redis Cluster / Kafka / Monitoring。生产环境不应成为开发阶段必要依赖。

## 32. 最终交付

Source Code / Build System / Protocol / DB Schema / Lua Scripts / Client / Server / Tests / Benchmarks / Bot / Docker / Local K8s / Deployment / Architecture Docs / Capacity Report / Failure Recovery Guide。
最终必须能：本地编译 / 本地启动 / 本地测试 / 本地压测 / 本地模拟故障。

## 33. 不得违反的核心红线

```
禁止 GameNode 直连 MySQL
禁止 战斗 Tick 写 DB
禁止 战斗 Tick 同步 Redis
禁止 战斗 Tick 同步 RPC
禁止 战斗 Tick 同步 Kafka
禁止 O(N) 全服广播
禁止 全局共享实时状态
禁止 全局锁作为核心并发模型
禁止 跨模块访问内部成员
禁止 循环依赖
禁止 无 Version 的关键状态写入
禁止 无 IdempotencyKey 的关键重试操作
禁止 运行中的 Tick 热切 Lua
禁止 Redis RDB 作为唯一状态恢复方案
禁止 为“微服务化”增加不必要进程
```

## 34. 最终架构图

```
                  Client
                     │
                 Protocol
                     │
                  Gateway
                     │
                   gRPC
                     │
        ┌────────────┼────────────┐
        │            │            │
     GameNode     GameNode     GameNode
        │            │            │
     ┌──┴────────────────────────┐
     │       Game Modules        │
     │ Scene / AOI / Movement    │
     │ Combat / Role / Quest     │
     │ Economy / Social / Lua    │
     └────────────┬──────────────┘
                  │
             DataService
              ┌───┴───┐
              │       │
            Redis   MySQL

             ControlService
```

## 35. 客户端必须从架构层面控制资源占用

禁止通过无限提高硬件要求解决性能问题。所有客户端系统优先考虑 CPU / RAM / VRAM / Draw Call / Texture Memory / Entity Count / Network Bandwidth / Loading Time。
必须提供 Low / Medium / High 三档。Low 模式作为主要兼容目标：4-core CPU / 4GB RAM / 1GB VRAM / 传统 DX11 GPU。
具体最低配置与实际 FPS 必须通过最终 Benchmark 确定，不得在未测试前宣称兼容某具体硬件。

---

## 变更流程（RFC）

1. 提出 Architecture Change Request，说明原因、影响范围、替代方案。
2. 经技术总监（@技术总监）人工批准。
3. 批准后由对应 Owner 修改本文件并同步更新 `ARCHITECTURE.md` / `DEVELOPMENT.md`。
4. 任何 AI / 开发成员**不得**在未批准情况下修改本文件。
