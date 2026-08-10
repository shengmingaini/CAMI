# Economy 经济模块详细设计

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 模块设计)  
> **所属层**: 逻辑业务层 (Game Node)  
> **上游规约**: `docs/architecture/architecture-spec.md` (v3.0.0, §4.2.2 / §1.2 经济风控)  
> **协议契约**: `docs/protocols/protocol-spec.md` + `proto/flatbuffers/economy.fbs`(✅已建立，flatc 编译校验通过) + `proto/protobuf/config_items.proto`(方案B)  
> **关联 ADR**: ADR-002（模块边界）、ADR-012（场景线程隔离）、ADR-013（三级缓存/货币入 L1）、ADR-003（拍卖行/公会独立库）、ADR-015（背压风控联动）

---

## 1. 模块概述

- **定位**：游戏内**货币、物品交易、商店、拍卖行、邮件附件**的权威交易中心与风控守门人。economy 是"发钱发物"的唯一受控出口——所有货币增减与物品增删最终都汇集到本模块，再统一经 `character` 落地，避免散落各模块造成经济数值失控。
- **核心职责**：
  1. 货币管理：多币种增减（经 `character.applyCurrencyDelta`），含上限/负值/溢出校验。
  2. 物品发放与回收：背包增删（经 character 背包接口），对象池化，支持批量。
  3. P2P 交易：玩家间交易（同场景）的锁定/确认/原子交换。
  4. 商店 NPC：buy/sell，读取 `config_items.proto`（`sell_price` / `stack_size` / `bind_type`）。
  5. 拍卖行：上架/竞拍/结算（**独立分片库**，ADR-003），跨场景/跨节点。
  6. 邮件系统：附件（物品/货币）发放与领取，离线可达。
  7. 风控防刷：交易频率、单笔阈值、经济事件速率，与背压（ADR-015）联动降级。
- **不在职责内**：不直接写 `PlayerCharacter` 字段（经 `ApplyXxx`）；不拥有 `Player`；不做战斗数值；不做聊天内容审核（由公共层 anti_cheat）。

## 2. 架构约束与边界（WoW 模式）

- **拥有 `Player` 对象**：否。
- **读契约**：通过 `const PlayerCharacter&` 读货币/背包/等级（校验交易/购买条件）。
- **写契约**：仅经由 character 落点 ——
  - 货币：`player->applyCurrencyDelta(CurrencyType, delta)`
  - 背包：经 character 背包接口（`addItem` / `removeItem`，内部做容量/绑定校验）
- **禁止**：`player->m_currency.x -= y` 直写；在交易/购买热路径同步写 DB（由数据层 WAL 异步）；任何模块旁路直接改货币（quest 发奖也必须经 economy）。

## 3. 对外接口（C++ 签名级）

命名空间 `cami::economy`。类 `EconomySystem`（每场景一个实例，运行于场景线程）。

### 3.1 权威发放入口（被 quest / social / GM 调用）
```cpp
// 任务/成就奖励发放（物品 + 货币 + 经验已由上游拆分）
bool grantReward(uint64_t player_id, const QuestReward& reward);
// 通用货币发放（声望/活动奖励等）
bool grantCurrency(uint64_t player_id, CurrencyType type, int64_t delta);
// 通用物品发放（带背包容量校验）
bool grantItem(uint64_t player_id, uint32_t item_id, uint32_t count);
```

### 3.2 玩家交互（由连接层意图事件触发）
```cpp
void onTradeIntent(const TradeIntentEvent& e);     // P2P 交易
void onShopBuy(const ShopBuyIntentEvent& e);       // 读 config_items.sell_price
void onShopSell(const ShopSellIntentEvent& e);
void onAuctionIntent(const AuctionIntentEvent& e); // 拍卖行（跨节点）
void onMailSend(const MailSendIntentEvent& e);     // 邮件附件
```

### 3.3 风控
```cpp
bool checkRateLimit(uint64_t player_id, EconomyOp op);  // 与背压 ADR-015 联动
bool checkThreshold(uint64_t player_id, int64_t delta);  // 单笔/累计阈值
```

## 4. 核心数据结构

- **交易上下文**：`TradeSession { buyer_id, seller_id, items[], currency, locked }`，对象池预分配。
- **拍卖列表**：`AuctionListing { listing_id, item_id, seller_id, bid, buyout, expire_ts }`，存于**独立分片库**（ADR-003，非 PlayerID 主库），跨场景只读副本缓存于 L2（ADR-013）。
- **邮件附件**：`MailAttachment { item_id[], currency[] }`，离线落地于数据层；领取时经本模块校验后调 character。
- **风控计数**：每玩家 `RateBucket`（滑动窗口），O(1) 更新，触发背压降级。

## 5. 事件契约（进程内 EventBus）

### 5.1 本模块发布的事件
| 事件 | 结构 | 消费者 |
|------|------|--------|
| `ItemGrantedEvent` | `{player_id, item_id, count, src}` | Quest/Social（反馈）、UI、AOI |
| `CurrencyGrantedEvent` | `{player_id, type, delta, new_total}` | 社交（富者榜）、UI、AOI |
| `TradeCompletedEvent` | `{buyer_id, seller_id, items[], currency}` | 社交、排行榜、日志 |
| `AuctionSettledEvent` | `{listing_id, buyer_id, seller_id, price}` | 邮件（打款）、排行榜 |
| `MailDeliveredEvent` | `{receiver_id, mail_id}` | UI、社交（通知） |

### 5.2 本模块订阅的事件
| 事件 | 发布者 | 处理逻辑 |
|------|--------|---------|
| `QuestTurnedInEvent {player_id, rewards}` | quest | 调用 `grantReward` 发奖 |
| `CurrencyChangedEvent` | character | 风控统计 / 富者榜更新 |
| `TradeIntentEvent` / `ShopBuy/SellIntent` / `AuctionIntent` / `MailSendIntent` | 连接处理层 | 进入交易/商店/拍卖/邮件流程 |

### 5.3 事件结构（示例）
```cpp
struct ItemGrantedEvent {
    uint64_t player_id;
    uint32_t item_id;
    uint32_t count;
    uint8_t  src;   // 0=quest,1=mail,2=shop,3=auction,4=gm
};
```

## 6. 协议引用

| 方向 | 消息类型（`CAMI.MessageBody`） | schema |
|------|-------------------------------|--------|
| C→S | `TradeIntent` / `ShopBuy` / `ShopSell` / `AuctionList` / `AuctionBid` / `MailSend` | `economy.fbs`(✅已建立) |
| S→C | `TradeResult` / `ShopResult` / `AuctionUpdate` / `MailList` / `CurrencyGranted` | `economy.fbs`(✅已建立) |

- 全部走**可靠轨道 B**（FlatBuffers + `MessageEnvelope`）；拍卖行行情分发可走轨道 A 差分（高频行情）。
- 商店定价只读 `config_items.proto.sell_price`，客户端不信任本地价格，服务端权威校验。

## 7. 并发模型

- **线程归属**：同场景交易/商店运行于**场景线程**（ADR-012），与 character 同线程无锁。
- **跨场景/跨节点**：拍卖行、跨服邮件、跨服交易经 **gRPC + Kafka（ADR-001）/ Redis Pub/Sub** + 独立分片库（ADR-003），本模块本地只持缓存副本，权威写入走数据层。
- **Job System**：邮件批量投递、拍卖行到期结算扫描、交易日志落库提交 Job System。

## 8. 性能预算

| 指标 | 红线 | 本模块保障 |
|------|------|-----------|
| 战斗循环(1000人) | ≤5ms | 经济操作不在战斗热路径；发放 O(1) |
| 单节点消息 | ≤20,000/s | 仅广播发放/交易结果（id + 量） |
| 货币操作延迟 | <0.1ms | 经 L1 缓存 + `applyCurrencyDelta` 内存写 |
| L1 缓存命中率 | ≥95% | 货币/热物品配置常驻进程内 |
| 拍卖行查询 | — | 独立分片库，不挤占主库；副本缓存 L2 |

## 9. 依赖方向

```
[quest] ──grantReward──→ [economy]
[social] ──grantCurrency/Item──→ [economy]
[连接处理层] ──Trade/Shop/Auction/Mail意图──→ [economy]
[economy] ──applyCurrencyDelta/背包增删──→ [character]  (唯一写入口, ADR-002)
[economy] ──拍卖/邮件跨节点──→ [数据层(独立库, ADR-003)] + [Redis Pub/Sub]
```
- **上游**：quest、social、连接处理层。
- **下游**：character（落地）、数据层（拍卖/邮件独立库）、config（config_items.proto）。
- **禁止逆向**：character 不得回调 economy；quest 不得绕过 economy 直发货币/物品。

## 10. 关联 ADR / 架构章节

| 决策 | 编号 |
|------|------|
| 模块边界 WoW 模式（不拥有 Player） | ADR-002 |
| 场景级线程隔离 + Job System | ADR-012 |
| 三级缓存 + 货币入 L1 | ADR-013 |
| 拍卖行/公会独立分片库 | ADR-003 |
| 多级背压与风控降级 | ADR-015 |

## 11. 开放问题 / 后续

- 拍卖行/邮件跨节点权威写入的具体 gRPC 接口与幂等键（架构 §13 #1 跨服事件，P1）待编码阶段落。
- `economy.fbs` 协议 schema 已于 2026-08-12 建立并经 flatc 编译校验通过（环境已装 flatc v25.12.19 + protoc）；编码阶段按 §6 对接即可。
- 风控阈值（单笔/频率/累计）需与数值策划对齐，建议在 config-center 配置化（方案 B 结构可扩展 `EconomyConfig`）。
- 邮件/拍卖行的数据层分片表 schema（独立库）需与 ADR-003 对齐并在数据层模块设计中落地。
