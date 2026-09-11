#pragma once

/// TASK-039 · SocialSystem 公开接口（§7 / §27.1，冻结契约）。
///
/// 状态归属（§4）：社交状态运行时权威归 SocialSystem（单 GameNode 内全量内存态）；
/// 跨节点由 Gateway 路由 + DataService 持久化副本兜底。公会/邮件持久化经
/// mmo::data::IDataStore（TASK-028）接口，禁止直连 MySQL / Redis。
///
/// 所有社交写操作经注册表（SocialOp → handler）分发 + EventBus 发布，
/// 禁止直接改 Role / Scene 私有数据（§21）。

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/data/idata_store.h"
#include "mmo/game/social/social_config.h"
#include "mmo/game/social/social_events.h"
#include "mmo/game/social/social_types.h"

namespace mmo::game::social {

class SocialSystem {
public:
    explicit SocialSystem(mmo::core::EventBus& bus,
                          mmo::data::IDataStore& store,
                          SocialConfig cfg = SocialConfig{});

    // ---- Party（§15.2） ----
    mmo::core::Result<party_id> CreateParty(player_id leader);
    mmo::core::Result<void>     Invite(player_id from, player_id to, party_id pid);
    mmo::core::Result<void>     AcceptInvite(player_id who, party_id pid);
    mmo::core::Result<void>     DeclineInvite(player_id who, party_id pid);
    mmo::core::Result<void>     Leave(player_id who);
    mmo::core::Result<void>     Kick(player_id leader, player_id target, party_id pid);
    mmo::core::Result<void>     Promote(player_id leader, player_id target, party_id pid);

    // ---- Friend（§15.3，双向一致，持久化经 DataService） ----
    mmo::core::Result<void> AddFriend(player_id a, player_id b);
    mmo::core::Result<void> RemoveFriend(player_id a, player_id b);

    // ---- Guild（§15.4，内存态 + 持久化双写） ----
    mmo::core::Result<guild_id> CreateGuild(player_id leader, std::string_view name);
    mmo::core::Result<void>     JoinGuild(player_id who, guild_id gid);
    mmo::core::Result<void>     LeaveGuild(player_id who, guild_id gid);

    // ---- Chat（§15.5，四频道；世界频道单条发布，禁 O(N) 广播） ----
    mmo::core::Result<void> SendChat(player_id from, ChatChannel channel,
                                     std::string_view message,
                                     player_id to = 0, party_id pid = 0, guild_id gid = 0);

    // ---- Mail（§15.6，附件走 Economy 幂等通道；领取幂等） ----
    mmo::core::Result<mail_id> SendMail(player_id from, player_id to,
                                        std::string_view subject, std::string_view body,
                                        bool has_attachment = false,
                                        std::uint64_t attachment_amount = 0);
    mmo::core::Result<void>    ClaimMail(player_id who, mail_id mid);

    // ---- 只读查询 ----
    Party* FindParty(party_id pid) noexcept;
    Guild* FindGuild(guild_id gid) noexcept;
    const std::set<player_id>* FriendsOf(player_id p) const noexcept;
    bool IsFriend(player_id a, player_id b) const noexcept;
    std::vector<mail_id> MailboxOf(player_id p) const;

    // ---- 运维指标 ----
    std::size_t PendingRetries() const noexcept { return pending_guild_saves_; }

private:
    // 注册表分发（禁 switch）。handler 以 this 绑定，注册于构造期。
    using Handler = std::function<mmo::core::Result<SocialOutcome>(const SocialCommand&)>;
    void RegisterBuiltins();
    mmo::core::Result<SocialOutcome> Dispatch(const SocialCommand& cmd);

    mmo::core::Result<SocialOutcome> HPartyCreate(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HInvite(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HAccept(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HDecline(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HLeave(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HKick(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HPromote(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HAddFriend(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HRemoveFriend(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HSendMail(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HClaimMail(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HGuildCreate(const SocialCommand&);
    mmo::core::Result<SocialOutcome> HJoinGuild(const SocialCommand&);

    // 持久化辅助（经 IDataStore；失败标记待重试，不丢内存态）
    void PersistGuild(const Guild& g);
    void PersistMail(const Mail& m);
    void PersistFriends(player_id p);

    mmo::core::EventBus& bus_;
    mmo::data::IDataStore& store_;
    SocialConfig cfg_;

    std::unordered_map<party_id, Party> parties_;
    std::unordered_map<player_id, std::set<player_id>> friends_;
    std::unordered_map<guild_id, Guild> guilds_;
    std::unordered_map<mail_id, Mail> mails_;
    std::unordered_map<player_id, std::vector<mail_id>> mailbox_;

    std::unordered_map<player_id, party_id> player_party_;   // 玩家当前所在队伍
    std::unordered_map<player_id, guild_id> player_guild_;   // 玩家当前所在公会
    std::unordered_map<player_id, std::vector<party_id>> invites_;  // 待接受邀请

    std::unordered_map<SocialOp, Handler> handlers_;

    party_id next_party_id_{1};
    guild_id next_guild_id_{1};
    mail_id  next_mail_id_{1};

    std::uint64_t pending_guild_saves_{0};  // 持久化失败待重试计数
};

}  // namespace mmo::game::social
