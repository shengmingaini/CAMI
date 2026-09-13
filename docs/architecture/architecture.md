# 架构总览（TASK-038 交付 · docs/architecture/architecture.md）

> 本文件是项目最终交付的架构基线文档，与 `ARCHITECTURE.md` / `PROJECT_REQUIREMENTS.md` 对齐。
> 架构冻结，任何结构性变更须经 RFC + 人工批准。

## 1. 进程拓扑

```
                 Client (协议层)
                     │  Protocol（Envelope: 4B 长度前缀 + payload）
                     ▼
                 Gateway  ──gRPC──┬── GameNode (×N, 无单点)
                     │             │   Scene / AOI / Movement / Combat / Role /
                     │             │   Inventory / Quest / Social / Economy / Lua
                     │             └── 同进程模块，默认共置，可后续拆子进程
                     ▼
               DataService ──┬── Redis（Session/Cache/Routing/Ranking）
                              └── MySQL（最终持久化，8 logical shards）
               ControlService（节点管理/配置下发/健康/运维）
```

四类核心进程（§3）：**Gateway / GameNode / DataService / ControlService**。
GameNode 默认拆为独立*模块*而非独立进程（Module ≠ Process，§2 红线 1）。

## 2. 通信模型（§4）

统一三消息：**Command / Query / Event**。
- 同进程：C++ Interface + Command + Event。
- 跨进程：gRPC + Protobuf。
- 异步业务：EventBus / Kafka。
- 统一 Envelope（§5）：`MessageID / MessageType / Version / Source / Timestamp / TraceID / RequestID / Payload`，经济操作额外带 `TransactionID / IdempotencyKey`。

Bot（TASK-038）为**协议层客户端**，复用 `mmo::protocol`（TASK-005）做 Envelope 编解码；
连接用自带 winsock 客户端，采用与 `mmo::net` 对称的「4 字节大端长度前缀 + payload」帧格式，
因此可对接任意兼容网关（真实 Gateway 或 sim 模式内嵌 MockGateway）。

## 3. 状态归属（§6 / §10）

Scene 是实时状态（Position/HP/MP/Buff/CombatState/MovementState）的唯一权威 Owner。
Redis **不是**实时状态最终权威来源（§17）。同一实时状态同一时刻只有一个权威写入者。

## 4. Tick 与并发（§8）

GameNode 固定 **20Hz / 50ms per Tick**，顺序：`Input → Movement → AOI → Combat → Buff → Quest → Event → Replication`。
热路径（Movement/AOI/Combat/Buff/Skill/Position Update）单 Owner + 顺序执行，禁止全局锁作核心模型，
禁止热路径同步 MySQL/Redis/gRPC/Kafka/文件 IO（§9）。

## 5. 验收与容量（§23 / §24）

- 性能基准：1000 玩家 Scene，平均/P95 Tick < 5ms，P99 < 8ms（实测见 `capacity-report.md`）。
- 容量目标：50,000 CCU（逐级 100→500→1000→5000→10000→20000→50000）；未达标须如实记录上限。
- Bot 压测：8 种行为（Login/Move/Attack/Quest/Trade/Chat/Logout/Reconnect），单机目标 5000 Bot。

## 6. 交付清单（TASK-038 §23）

`tools/{bot,load,chaos,netsim,report}` + `docs/architecture/{architecture,state-ownership,
dependency,capacity-report}.md` + `sequence/` + `deployment/` + `deploy/{docker,k8s}` +
`README/DEPLOYMENT/OPERATIONS`。详见各子文档。
