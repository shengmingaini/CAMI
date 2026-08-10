# CAMI 网络通信协议总规约

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 协议基础优先)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, 16 ADR)  
> **配套 schema**: `proto/flatbuffers/*.fbs` (可靠轨) + `proto/protobuf/*.proto` (跨服/配置)

---

## 1. 设计目标与约束

协议栈必须满足架构红线（见架构规约 §1.2）：

| 约束 | 指标 | 协议侧对应手段 |
|------|------|----------------|
| 单节点消息吞吐 | ≤20,000 条/s | 双轨分流：高频移动/朝向走无确认 UDP 位压缩，可靠消息走 QUIC 流 |
| 高频消息带宽(1000人) | ≤50 KB/s | 位压缩 + 差分增量 + LOD 裁剪 |
| 客户端重连 | ~0ms | QUIC 0-RTT + 连接迁移（连接 ID 不变） |
| Gateway↔GameNode(同节点) | <0.05ms | 共享内存 SPSC RingBuffer（零拷贝/零序列化） |
| 跨节点 AOI 消息量 | ≤10 条/s | AOI 跨节点聚合 + 差分压缩（架构 §4.2.4） |
| 预测正确率 | ≥95% | 客户端预测 + 服务器权威校验 + 纠偏（架构 §4.2.11） |

**硬性禁令（继承自架构 §1.3）**：
1. 禁止 JSON 传高频战斗数据 → 全部走二进制（FlatBuffers / 位压缩）
2. 禁止无 AOI 全服广播 → 消息必须经 AOI 作用域过滤
3. 禁止战斗循环内写 DB → 协议层不携带持久化指令，持久化由数据层异步处理

---

## 2. 分层与传输拓扑

```
                      ┌──────────────┐
   客户端 (移动/PC) ───┤ 边缘网关      │  QUIC (0-RTT, 连接迁移)   [高频: UDP 位压缩]
                      │ Edge GW      │
                      └──────┬───────┘
                             │ 专线转发 (私有链路, 内网)
                      ┌──────┴───────┐
                      │ 中心网关      │  QUIC 终结 + 协议编解码
                      │ Center GW    │
                      └──┬───────┬──┘
           同节点        │       │        跨节点
        ┌──────────────┘       └──────────────┐
  共享内存 SHM           gRPC (批量, <1ms)
  SPSC RingBuffer        ┌────────────────────┴──────┐
  (<0.05ms, 零拷贝)      │  GameNode (场景线程隔离)    │
                         │  Scene/Character/Combat/AOI│
                         └──────────┬─────────────────┘
                    GameNode ↔ Redis 直连 (CRC16, <0.5ms) [v3.0]
                    GameNode ↔ DataService (gRPC, <2ms) 仅 MySQL 写入
```

**规则**：
- 客户端只与**边缘网关**建立 QUIC 连接；边缘网关无状态，仅做就近接入 + 加密终结 + 专线转发。
- 中心网关是唯一做**协议编解码**的边界；Gateway 内部零玩家数据持久化。
- GameNode 内模块间一律走进程内 EventBus（架构 §4.2），不走网络。

---

## 3. 双轨消息模型（核心设计）

| 维度 | 轨道 A：高频 unreliable | 轨道 B：可靠 reliable |
|------|------------------------|----------------------|
| 承载 | 移动/朝向/技能意图/AOI 增量 | 登录/交易/任务/背包/邮件/社交/战斗结果 |
| 传输 | UDP datagram（QUIC 0-RTT 亦可，按延迟选） | QUIC 流（有序、加密、重传） |
| 编码 | **自定义位压缩**（非 FlatBuffers） | **FlatBuffers**（见 `proto/flatbuffers/`） |
| 确认 | 无（丢包靠下一帧覆盖） | QUIC 流保证有序到达 |
| 加密 | AES-128-GCM，截断 8 字节 tag | AES-128-GCM，16 字节 tag |
| 典型频率 | 10–30 Hz/实体 | 事件驱动（远低于轨道 A） |
| 设计依据 | 架构 §4.1 双轨 + §4.2.4 LOD | 架构 §3.3 通信矩阵 |

> **为什么高频轨道不用 FlatBuffers**：FlatBuffers 仍需表头/偏移开销（每消息 ~8–16B 固定成本），而位压缩移动包可压到 5–15B。高频 1000 人 × 20Hz 下，位压缩相比 FlatBuffers 节省约 60% 带宽，是 ≤50KB/s 红线的关键手段。

---

## 4. 消息信封格式

### 4.1 轨道 B 信封（FlatBuffers）

可靠消息以 `MessageEnvelope`（见 `proto/flatbuffers/envelope.fbs`）为统一容器：

```
MessageEnvelope {
  seq         : uint32           // 单调递增序号（每连接每方向独立，防重放见 §6.3）
  request_id  : uint32           // 请求/响应关联（0 = 单向广播）
  flags       : uint8            // bit0=加密, bit1=压缩, bit2=需要响应
  body        : MessageBody      // union；实际类型由自动生成的 body_type 字段判别
}
```

- **类型判别**：FlatBuffers 为 union 自动生成枚举 `CAMI.MessageBody`（含 `NONE=0` 与各成员），网关/GameNode 读取 `body_type` 决定如何解析 `body`，无需额外数值 ID（§5 目录按逻辑域分组）。
- **长度前缀**：QUIC 流上每帧前加 4 字节大端 `uint32` 长度，便于流式分包。
- **加密范围**：`body` 字段密文 + `seq`/`request_id` 明文（便于乱序解密）+ GCM tag。

### 4.2 轨道 A 信封（位压缩）

高频包采用紧凑二进制布局（codec 模块实现，见 §7 示例）：

```
[6-bit opcode][1-bit hasSeq][1-bit isDelta] | [seq:16 if hasSeq] | [payload: 位域...]
```

- `opcode` 复用 §5 中"高频域"映射（6 位可表达 64 类高频消息）。
- 位置增量用 **delta + 变长位宽**（如 ±3m 内用 8 位有符号，超出用 16 位）。
- 朝向用 1 字节量化角度（360°/256）。

---

## 5. 消息目录（Message Catalog）

消息按「域」分组。**轨道 B** 用 FlatBuffers union 枚举 `CAMI.MessageBody` 判别类型（union 成员顺序即枚举值）；**轨道 A** 用 6-bit opcode（见 §4.2 / §7）。新增消息须落在其逻辑域内，禁止跨域复用。

| 域 | 逻辑消息 | 轨道 B 类型 (`CAMI.MessageBody`) | 轨道 A opcode |
|----|---------|-------------------------------|---------------|
| 系统/连接 | 客户端握手 | `ClientHello` | — |
| 系统/连接 | 服务端握手 | `ServerHello` | — |
| 系统/连接 | 心跳 | `Heartbeat` | — |
| 系统/连接 | 断开通知 | `DisconnectNotify` | — |
| 系统/连接 | 连接迁移 | `MigrateNotify` | — |
| 角色 | 登录请求 | `LoginRequest` | — |
| 角色 | 登录响应 | `LoginResponse` | — |
| 角色 | 进入世界请求 | `EnterWorldRequest` | — |
| 角色 | 进入世界响应 | `EnterWorldResponse` | — |
| 移动/AOI | 全量位置快照 | `MovementSnapshot` | — |
| 移动/AOI | 服务器纠偏 | `Correction` | — |
| 移动/AOI | AOI 进入 | `AoiEnter` | — |
| 移动/AOI | AOI 离开 | `AoiLeave` | — |
| 移动/AOI | AOI 增量 | `AoiUpdate` | — |
| 战斗 | 施法意图 | `SkillCastIntent` | 可选（高频） |
| 战斗 | 施法结果 | `SkillCastResult` | — |
| 战斗 | 伤害事件 | `DamageEvent` | — |
| 战斗 | Buff 事件 | `BuffEvent` | — |
| 战斗 | 战斗结果 | `CombatResult` | — |
| 任务 | 接取意图 | `QuestAcceptIntent` | — |
| 任务 | 交付意图 | `QuestTurnInIntent` | — |
| 任务 | 进度增量 | `QuestProgress` | — |
| 任务 | 奖励结果 | `QuestRewardResult` | — |
| 任务 | 任务日志全量 | `QuestLogSnapshot` | — |
| 经济 | 交易意图 | `TradeIntent` | — |
| 经济 | 商店购买 | `ShopBuy` | — |
| 经济 | 商店出售 | `ShopSell` | — |
| 经济 | 拍卖上架 | `AuctionList` | — |
| 经济 | 拍卖出价 | `AuctionBid` | — |
| 经济 | 邮件发送 | `MailSend` | — |
| 经济 | 交易结果 | `TradeResult` | — |
| 经济 | 商店结果 | `ShopResult` | — |
| 经济 | 拍卖行情 | `AuctionUpdate` | — |
| 经济 | 邮件列表 | `MailList` | — |
| 经济 | 货币发放 | `CurrencyGranted` | — |
| 社交 | 加好友 | `FriendAdd` | — |
| 社交 | 建公会 | `GuildCreate` | — |
| 社交 | 组队邀请 | `PartyInvite` | — |
| 社交 | 聊天发送 | `ChatSend` | — |
| 社交 | 好友列表 | `FriendList` | — |
| 社交 | 公会信息 | `GuildInfo` | — |
| 社交 | 队伍更新 | `PartyUpdate` | — |
| 社交 | 聊天广播 | `ChatBroadcast` | — |
| 社交 | 成就更新 | `AchievementUpdate` | — |
| 社交 | 声望更新 | `ReputationUpdate` | — |

> 轨道 A 高频消息（移动增量/朝向）的 6-bit opcode 分配与位压缩布局见 §7 示例；完整 opcode 表在编码阶段随 codec 模块落地。任务/经济/社交三域 IDL 已于 2026-08-12 补齐（见 `quest.fbs`/`economy.fbs`/`social.fbs` 与 `protocol-manifest-v1.md`）。**仍 pending**：背包/Inventory 独立消息、场景/Scene 管理消息、寻路/Navigation 消息、跨服完整事件集（架构 §13 #1），将在对应层模块设计阶段补充。

---

## 6. 加密与认证（AES-128-GCM）

### 6.1 握手建立会话密钥

```
C → S: ClientHello  { x25519_pub_client, client_nonce(12B), proto_version }
S → C: ServerHello  { x25519_pub_server, server_nonce(12B), token/时间戳签名 }
双方: shared = X25519(pri, peer_pub)
     master = HKDF-SHA256(shared || client_nonce || server_nonce)
     aes_key(16B) = master[0:16]
     send_nonce_seed / recv_nonce_seed 由 master 派生
```

### 6.2 每消息加密

- **算法**：AES-128-GCM。
- **Nonce**：12 字节，每方向独立单调递增（禁止复用，复用即灾难性失败）。
- **Tag**：
  - 轨道 B：完整 16 字节。
  - 轨道 A：截断为 8 字节（高频包容忍极低概率伪造，换取带宽）。
- **IV 构造**：`nonce = seed XOR counter_64`，counter 随 `seq` 递增。
- **密钥轮换**：每 24 小时或每 2^32 条消息强制重协商（防 nonce 耗尽）。

### 6.3 防重放

- 接收方维护最近 **N=1024** 个 `seq` 的滑动窗口，丢弃 `seq ≤ window_min` 的包。

---

## 7. 高频轨道 A 编码示例（移动增量）

约定：世界坐标用定点（1 单位 = 1cm），增量用有符号变长。

```
移动增量 (opcode=0x01, isDelta=1):
  bit[0:5]   opcode = 1
  bit[6]     hasSeq = 1
  bit[7]     isDelta = 1
  byte[1:2]  seq (uint16, 每实体独立)
  byte[3]    moveState (0静止/1走/2跑/3跳)  // 见 common.fbs MoveState
  byte[4:5]  dx (int16, cm)                 // 相对上一帧
  byte[6:7]  dy (int16, cm)
  byte[8:9]  dz (int16, cm)
  byte[10]   yaw (uint8, 量化角度)
  → 总 11 字节（含 1B opcode），远低于 FlatBuffers 等价 ~24B
```

> 绝对位置仅在"进入 AOI / 纠偏"时用轨道 B 的 `MovementSnapshot` 全量下发。

---

## 8. 连接生命周期

| 阶段 | 消息 | 说明 |
|------|------|------|
| 握手 | 0x0001/0x0002 | §6.1 密钥协商；失败直接断连 |
| 登录 | 0x1001/0x1002 | 角色列表 → 进入世界；加载 Player 到内存（架构 §4.2.2） |
| 在线 | 轨道 A/B 混合 | 高频移动走 A，业务走 B |
| 心跳 | 0x000A | 30s 一次（QUIC 自带 PING 亦可） |
| 迁移 | 0x000C | GameNode 宕机时连接上下文迁移（QUIC 连接 ID 不变，<800ms，v3.0 0-RTT） |
| 断开 | 0x000B | 客户端发起；服务器立即 WAL flush 持久化（架构 §4.3） |

---

## 9. 可靠性与序号

- **轨道 B**：依赖 QUIC 流有序、可靠、去重，应用层不重复实现 ACK。仅需 `request_id` 做请求/响应关联（如 0x1001 登录请求 → 0x1002 响应带同 `request_id`）。
- **轨道 A**：无确认；移动/朝向丢包由下一帧自然覆盖；AOI 进入/离开用轨道 B 保证最终一致。
- **跨服事件（0xA000 域）**：走 Redis Pub/Sub 或 gRPC，由对应机制保证（见架构 §3.3）。

---

## 10. 性能预算（验收基线）

| 指标 | 目标 | 协议侧保障 |
|------|------|-----------|
| 单节点消息吞吐 | ≤20,000/s | 双轨分流 + 位压缩 |
| 高频带宽(1000人) | ≤50 KB/s | 位压缩 + 差分 + LOD 裁剪（架构 §4.2.4） |
| Gateway↔GameNode 同节点 | <0.05ms | SHM RingBuffer |
| 客户端重连 | ~0ms | QUIC 0-RTT |
| 跨节点 AOI | ≤10 条/s | 聚合 + 差分（架构 §4.2.4） |
| 加密单包开销 | ≤16B(B)/≤9B(A) | GCM tag（A 截断 8B） |

---

## 11. 与架构 ADR 的对应关系

| 协议决策 | 关联 ADR/章节 |
|----------|--------------|
| QUIC + 0-RTT 迁移 | 架构 §4.1 连接迁移 / v3.0 |
| 双轨（位压缩 + FlatBuffers） | 架构 §4.1 编解码 / §4.2.4 |
| 共享内存 IPC 替代 gRPC | 架构 ADR-13 (v3.0) |
| 三级缓存 L1/L2/L3 | 架构 ADR-15 (v3.0) |
| 4 级背压 | 架构 ADR-17 (v3.0) — 协议层配合降频 |
| 差分压缩 + LOD | 架构 §4.2.4 (v2.0/v3.0) |

---

## 12. 后续（非本轮范围）

- 各模块详细设计文档（`docs/modules/`）将引用本规约的消息 ID 与信封。
- 配置表 / 数据驱动（物品/技能/任务）的 Protobuf schema 在本规约 `proto/protobuf/` 下扩展。
- codec 模块（位压缩编解码器）实现与单元测试为编码阶段任务。
