# Social System · INTERFACE.md（TASK-039）

模块路径：`server/gamenode/social/`
命名空间：`mmo::game::social`
单 GameNode 内全量内存态；跨节点社交由 Gateway 路由 + DataService 持久化副本兜底。

## 1. 导出头（公开接口，冻结契约）

| 头文件 | 导出内容 |
|---|---|
| `mmo/game/social/social_types.h` | `SocialOp`、`ChatChannel`、`Party`、`Guild`、`Mail`、`SocialCommand`、`SocialOutcome`、ID 别名（`player_id`/`party_id`/`guild_id`/`mail_id`） |
| `mmo/game/social/social_events.h` | `PartyEvent`、`GuildEvent`、`ChatEvent`、`MailEvent`（四类事件，值类型，经 EventBus 发布） |
| `mmo/game/social/social_config.h` | `SocialConfig`、`ParseSocialConfig(std::string_view)` |
| `mmo/game/social/social_system.h` | `SocialSystem` 类（全部公开方法，见 §3） |

> 内部头 `src/social_io.h`（序列化）与 `src/*.cpp` **不进公开接口**，验收脚本会静态扫描 `include/` 是否泄露内部 `src/`。

## 2. 消费的上游接口（§27.2，禁止绕过、禁止 include src/）

| 来源任务 | 模块 / 目标 | 消费符号 |
|---|---|---|
| TASK-007 | `engine/core`（`mmo::core_error` / `mmo::core_bus` / `mmo::core_time`） | `core::Result<T>`、`core::Error`、`core::ErrorCode`、`core::EventBus::Publish<T>` / `Subscribe<T>` / `Drain` |
| TASK-011 | `server/gamenode/entity`（`mmo::gamenode_entity`） | 复用 `EntityId` 等类型别名（仅类型，未持有实体对象） |
| TASK-012 | `server/gamenode/scene`（`mmo::gamenode_scene`） | `mmo::game::PlayerId` / `SceneId` / `NodeId`（**不重定义**） |
| TASK-028 | `server/dataservice`（`mmo::dataservice`） | `mmo::data::IDataStore`（含 `Record` / `DataKey` / `VersionCheck`）、`InMemoryStore`、`FakeDataStore` |

约束：所有社交写操作经 `IDataStore` 接口持久化，**禁止直连 MySQL / Redis**；禁止直接改 `Role` / `Scene` 私有数据。

## 3. SocialSystem 公开方法

```
SocialSystem(core::EventBus& bus, mmo::data::IDataStore& store, SocialConfig cfg = {});

// Party
core::Result<party_id> CreateParty(player_id leader);
core::Result<void>   Invite(player_id from, player_id to, party_id pid);
core::Result<void>   AcceptInvite(player_id who, party_id pid);
core::Result<void>   DeclineInvite(player_id who, party_id pid);
core::Result<void>   Leave(player_id who);
core::Result<void>   Kick(player_id leader, player_id target, party_id pid);
core::Result<void>   Promote(player_id leader, player_id target, party_id pid);

// Friend（双向一致，持久化经 DataStore）
core::Result<void> AddFriend(player_id a, player_id b);
core::Result<void> RemoveFriend(player_id a, player_id b);

// Guild（内存态 + 持久化双写）
core::Result<guild_id> CreateGuild(player_id leader, std::string_view name);
core::Result<void>     JoinGuild(player_id who, guild_id gid);
core::Result<void>     LeaveGuild(player_id who, guild_id gid);

// Chat（四频道；世界频道单条事件，由 Gateway 订阅表路由，禁 O(N) 广播）
core::Result<void> SendChat(player_id from, ChatChannel channel,
                            std::string_view message,
                            player_id to = 0, party_id pid = 0, guild_id gid = 0);

// Mail（附件走 Economy 幂等通道；领取幂等）
core::Result<mail_id> SendMail(player_id from, player_id to,
                               std::string_view subject, std::string_view body,
                               bool has_attachment = false,
                               std::uint64_t attachment_amount = 0);
core::Result<void>    ClaimMail(player_id who, mail_id mid);

// 只读查询
Party* FindParty(party_id); Guild* FindGuild(guild_id);
const std::set<player_id>* FriendsOf(player_id);
bool IsFriend(player_id a, player_id b);
std::vector<mail_id> MailboxOf(player_id);
std::size_t PendingRetries() const;   // 持久化失败待重试计数（§19）
```

## 4. 注册表（替代 switch，§8 / §21）

`SocialSystem` 内部维护 `std::unordered_map<SocialOp, Handler>`，构造期 `RegisterBuiltins()`
将每个 `SocialOp` 绑定到一个 handler（`HInvite` / `HAccept` / …）。公开方法构造 `SocialCommand`
并调用 `Dispatch(cmd)` 查表分发。**无任何 `switch` 硬编码穷举社交类型**；新增社交关系类型
只需：① 在 `SocialOp` 枚举加值；② 注册对应 handler。本任务在规格 12 项基础上扩展 `PartyCreate`
（组队创建，§15.2），仍走同一注册表。

## 5. 事件（§7）

四类事件均为小值类型，通过 `core::EventBus::Publish<T>` 发布、由订阅者（Gateway / 会话层）消费：
- `PartyEvent`：创建 / 邀请 / 接受 / 拒绝 / 离开 / 踢人 / 队长转移。
- `GuildEvent`：创建 / 加入 / 离开。
- `ChatEvent`：私聊 / 队伍 / 公会 / 世界四频道。世界频道**仅发布单条事件**，接收者路由由
  Gateway 订阅表完成，SocialSystem 内绝不做全服玩家 O(N) 遍历广播。
- `MailEvent`：发送 / 领取。

## 6. 持久化键约定（经 `IDataStore::payload`）

| 键 | 内容 |
|---|---|
| `guild:<id>` | `SerializeGuild` → `name|leader|member_count|m1,m2,...` |
| `mail:<id>` | `SerializeMail` → 以 `\x1f` 分隔的 10 字段 |
| `friend:<player>` | `SerializeFriends` → 逗号分隔的 player 列表 |

写路径使用 `VersionCheck{required=false}`（简单 KV 覆盖）；版本由上层 DataService 管理。
Guild 持久化失败时内存态保留并 `PendingRetries()++`（标记待重试，不丢数据，§19）。

## 7. 配置（config/gameplay/social.json）

`ParseSocialConfig` 做极简字段扫描（无第三方 JSON 依赖）：`party_size_max` / `guild_member_max`
/ `mail_ttl_seconds` / `world_chat_enabled`。缺失字段回退默认值。

## 8. 测试与验收

- 测试可执行：`social_test`（ctest `-R Social` 命中 `Game_Social.Suite`）。
- 覆盖：组队生命周期 / 好友双向 / 公会双写一致性（内存 == 存储快照）/ 公会持久化失败保留内存态 /
  聊天四频道（世界频道单条发布）/ 邮件幂等领取（重复领取返回 AlreadyClaimed）。
- Debug / Release 双构建 0 警告；`ctest -R Social` 全绿。
