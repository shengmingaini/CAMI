// server/gamenode/social/src/social_system.cpp — TASK-039 SocialSystem 实现
//
// 注册表分发（禁 switch）+ EventBus 发布 + IDataStore 持久化。
// 禁止直接改 Role / Scene 私有数据；世界频道单条事件发布，禁全服 O(N) 广播。

#include "mmo/game/social/social_system.h"

#include <algorithm>
#include <chrono>

#include "mmo/core/error/error.h"
#include "social_io.h"

namespace mmo::game::social {

using namespace mmo::core;  // Error / ErrorCode / Result / domain

namespace {
constexpr std::uint64_t kZeroU64 = 0ULL;
constexpr std::string_view kSocialDomain = "social";
inline Error SocErr(ErrorCode code, std::string_view msg) {
    return Error(code, msg, kSocialDomain);
}
}  // namespace

// ---------------------------------------------------------------------------
// 构造 + 注册表
// ---------------------------------------------------------------------------

SocialSystem::SocialSystem(mmo::core::EventBus& bus,
                           mmo::data::IDataStore& store,
                           SocialConfig cfg)
    : bus_(bus), store_(store), cfg_(cfg) {
    RegisterBuiltins();
}

void SocialSystem::RegisterBuiltins() {
    handlers_[SocialOp::PartyCreate]  = [this](const SocialCommand& c) { return HPartyCreate(c); };
    handlers_[SocialOp::Invite]       = [this](const SocialCommand& c) { return HInvite(c); };
    handlers_[SocialOp::Accept]       = [this](const SocialCommand& c) { return HAccept(c); };
    handlers_[SocialOp::Decline]      = [this](const SocialCommand& c) { return HDecline(c); };
    handlers_[SocialOp::Leave]        = [this](const SocialCommand& c) { return HLeave(c); };
    handlers_[SocialOp::Kick]         = [this](const SocialCommand& c) { return HKick(c); };
    handlers_[SocialOp::Promote]      = [this](const SocialCommand& c) { return HPromote(c); };
    handlers_[SocialOp::AddFriend]    = [this](const SocialCommand& c) { return HAddFriend(c); };
    handlers_[SocialOp::RemoveFriend] = [this](const SocialCommand& c) { return HRemoveFriend(c); };
    handlers_[SocialOp::SendMail]     = [this](const SocialCommand& c) { return HSendMail(c); };
    handlers_[SocialOp::ClaimMail]    = [this](const SocialCommand& c) { return HClaimMail(c); };
    handlers_[SocialOp::GuildCreate]  = [this](const SocialCommand& c) { return HGuildCreate(c); };
    handlers_[SocialOp::GuildJoin]    = [this](const SocialCommand& c) { return HJoinGuild(c); };
}

Result<SocialOutcome> SocialSystem::Dispatch(const SocialCommand& cmd) {
    auto it = handlers_.find(cmd.op);
    if (it == handlers_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "unknown social op"));
    }
    return it->second(cmd);
}

// ---------------------------------------------------------------------------
// Party（§15.2）
// ---------------------------------------------------------------------------

Result<party_id> SocialSystem::CreateParty(player_id leader) {
    if (leader == kZeroU64) {
        return Result<party_id>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid leader"));
    }
    SocialCommand c;
    c.op = SocialOp::PartyCreate;
    c.actor = leader;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<party_id>::Fail(out.Err());
    return Result<party_id>::Ok(out.Value().as_party);
}

Result<SocialOutcome> SocialSystem::HPartyCreate(const SocialCommand& c) {
    const party_id pid = next_party_id_++;
    Party p;
    p.id = pid;
    p.leader = c.actor;
    p.members.push_back(c.actor);
    parties_[pid] = std::move(p);
    player_party_[c.actor] = pid;
    SocialOutcome o;
    o.has_party = true;
    o.as_party = pid;
    (void)bus_.Publish(PartyEvent{SocialOp::PartyCreate, pid, c.actor, c.actor, kZeroU64});
    return Result<SocialOutcome>::Ok(o);
}

Result<void> SocialSystem::Invite(player_id from, player_id to, party_id pid) {
    SocialCommand c;
    c.op = SocialOp::Invite;
    c.actor = from;
    c.target = to;
    c.pid = pid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HInvite(const SocialCommand& c) {
    auto pit = parties_.find(c.pid);
    if (pit == parties_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "party not found"));
    }
    Party& p = pit->second;
    if (p.leader != c.actor) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::UNAUTHORIZED, "not party leader"));
    }
    if (c.target == kZeroU64) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid target"));
    }
    if (p.members.size() >= static_cast<std::size_t>(cfg_.party_size_max)) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "party full"));
    }
    if (std::find(p.members.begin(), p.members.end(), c.target) != p.members.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "already in party"));
    }
    auto iit = invites_.find(c.target);
    if (iit != invites_.end() &&
        std::find(iit->second.begin(), iit->second.end(), c.pid) != iit->second.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "already invited"));
    }
    invites_[c.target].push_back(c.pid);
    (void)bus_.Publish(PartyEvent{SocialOp::Invite, c.pid, c.actor, c.target, p.home});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::AcceptInvite(player_id who, party_id pid) {
    SocialCommand c;
    c.op = SocialOp::Accept;
    c.actor = who;
    c.pid = pid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HAccept(const SocialCommand& c) {
    auto iit = invites_.find(c.actor);
    if (iit == invites_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "no pending invite"));
    }
    auto& vec = iit->second;
    auto f = std::find(vec.begin(), vec.end(), c.pid);
    if (f == vec.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "no invite for party"));
    }
    vec.erase(f);
    if (vec.empty()) invites_.erase(iit);

    auto pit = parties_.find(c.pid);
    if (pit == parties_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "party gone"));
    }
    Party& p = pit->second;
    if (p.members.size() >= static_cast<std::size_t>(cfg_.party_size_max)) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "party full"));
    }
    p.members.push_back(c.actor);
    player_party_[c.actor] = c.pid;
    (void)bus_.Publish(PartyEvent{SocialOp::Accept, c.pid, c.actor, c.actor, p.home});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::DeclineInvite(player_id who, party_id pid) {
    SocialCommand c;
    c.op = SocialOp::Decline;
    c.actor = who;
    c.pid = pid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HDecline(const SocialCommand& c) {
    auto iit = invites_.find(c.actor);
    if (iit == invites_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "no pending invite"));
    }
    auto& vec = iit->second;
    auto f = std::find(vec.begin(), vec.end(), c.pid);
    if (f == vec.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "no invite for party"));
    }
    vec.erase(f);
    if (vec.empty()) invites_.erase(iit);
    (void)bus_.Publish(PartyEvent{SocialOp::Decline, c.pid, c.actor, c.actor, kZeroU64});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::Leave(player_id who) {
    SocialCommand c;
    c.op = SocialOp::Leave;
    c.actor = who;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HLeave(const SocialCommand& c) {
    auto pit = player_party_.find(c.actor);
    if (pit == player_party_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "not in a party"));
    }
    const party_id pid = pit->second;
    auto mit = parties_.find(pid);
    player_party_.erase(pit);
    if (mit == parties_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "party gone"));
    }
    Party& p = mit->second;
    const scene_id home = p.home;
    auto f = std::find(p.members.begin(), p.members.end(), c.actor);
    if (f != p.members.end()) p.members.erase(f);
    if (p.members.empty()) {
        parties_.erase(mit);
    } else if (p.leader == c.actor) {
        p.leader = p.members.front();  // 自动转移队长，避免悬空领导
    }
    (void)bus_.Publish(PartyEvent{SocialOp::Leave, pid, c.actor, c.actor, home});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::Kick(player_id leader, player_id target, party_id pid) {
    SocialCommand c;
    c.op = SocialOp::Kick;
    c.actor = leader;
    c.target = target;
    c.pid = pid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HKick(const SocialCommand& c) {
    auto pit = parties_.find(c.pid);
    if (pit == parties_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "party not found"));
    }
    Party& p = pit->second;
    if (p.leader != c.actor) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::UNAUTHORIZED, "not party leader"));
    }
    if (c.target == kZeroU64) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid target"));
    }
    auto f = std::find(p.members.begin(), p.members.end(), c.target);
    if (f == p.members.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "not a member"));
    }
    p.members.erase(f);
    player_party_.erase(c.target);
    if (p.members.empty()) parties_.erase(pit);
    (void)bus_.Publish(PartyEvent{SocialOp::Kick, c.pid, c.actor, c.target, p.home});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::Promote(player_id leader, player_id target, party_id pid) {
    SocialCommand c;
    c.op = SocialOp::Promote;
    c.actor = leader;
    c.target = target;
    c.pid = pid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HPromote(const SocialCommand& c) {
    auto pit = parties_.find(c.pid);
    if (pit == parties_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "party not found"));
    }
    Party& p = pit->second;
    if (p.leader != c.actor) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::UNAUTHORIZED, "not party leader"));
    }
    if (c.target == c.actor) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "already leader"));
    }
    if (std::find(p.members.begin(), p.members.end(), c.target) == p.members.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "not a member"));
    }
    p.leader = c.target;
    (void)bus_.Publish(PartyEvent{SocialOp::Promote, c.pid, c.actor, c.target, p.home});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

// ---------------------------------------------------------------------------
// Friend（§15.3）
// ---------------------------------------------------------------------------

Result<void> SocialSystem::AddFriend(player_id a, player_id b) {
    SocialCommand c;
    c.op = SocialOp::AddFriend;
    c.actor = a;
    c.target = b;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HAddFriend(const SocialCommand& c) {
    if (c.actor == kZeroU64 || c.target == kZeroU64 || c.actor == c.target) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid friend pair"));
    }
    friends_[c.actor].insert(c.target);
    friends_[c.target].insert(c.actor);
    PersistFriends(c.actor);
    PersistFriends(c.target);
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::RemoveFriend(player_id a, player_id b) {
    SocialCommand c;
    c.op = SocialOp::RemoveFriend;
    c.actor = a;
    c.target = b;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HRemoveFriend(const SocialCommand& c) {
    auto ait = friends_.find(c.actor);
    if (ait != friends_.end()) {
        ait->second.erase(c.target);
        if (ait->second.empty()) friends_.erase(ait);
    }
    auto bit = friends_.find(c.target);
    if (bit != friends_.end()) {
        bit->second.erase(c.actor);
        if (bit->second.empty()) friends_.erase(bit);
    }
    PersistFriends(c.actor);
    PersistFriends(c.target);
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

// ---------------------------------------------------------------------------
// Guild（§15.4）
// ---------------------------------------------------------------------------

Result<guild_id> SocialSystem::CreateGuild(player_id leader, std::string_view name) {
    SocialCommand c;
    c.op = SocialOp::GuildCreate;
    c.actor = leader;
    c.payload = std::string(name);
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<guild_id>::Fail(out.Err());
    return Result<guild_id>::Ok(out.Value().as_guild);
}

Result<SocialOutcome> SocialSystem::HGuildCreate(const SocialCommand& c) {
    if (c.actor == kZeroU64) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid leader"));
    }
    if (c.payload.empty()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "empty guild name"));
    }
    if (c.payload.find('|') != std::string::npos) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid char in name"));
    }
    const guild_id gid = next_guild_id_++;
    const std::string gname = c.payload;  // 拷贝，避免后续 move 后访问悬空
    Guild g;
    g.id = gid;
    g.name = gname;
    g.leader = c.actor;
    g.member_count = 1U;
    g.members.push_back(c.actor);
    guilds_[gid] = std::move(g);
    player_guild_[c.actor] = gid;
    PersistGuild(guilds_[gid]);
    SocialOutcome o;
    o.has_guild = true;
    o.as_guild = gid;
    (void)bus_.Publish(GuildEvent{SocialOp::GuildCreate, gid, c.actor, kZeroU64, gname});
    return Result<SocialOutcome>::Ok(o);
}

Result<void> SocialSystem::JoinGuild(player_id who, guild_id gid) {
    SocialCommand c;
    c.op = SocialOp::GuildJoin;
    c.actor = who;
    c.gid = gid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HJoinGuild(const SocialCommand& c) {
    auto git = guilds_.find(c.gid);
    if (git == guilds_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "guild not found"));
    }
    Guild& g = git->second;
    if (g.members.size() >= static_cast<std::size_t>(cfg_.guild_member_max)) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "guild full"));
    }
    if (std::find(g.members.begin(), g.members.end(), c.actor) != g.members.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "already member"));
    }
    g.members.push_back(c.actor);
    g.member_count = static_cast<std::uint32_t>(g.members.size());
    player_guild_[c.actor] = c.gid;
    PersistGuild(g);
    (void)bus_.Publish(GuildEvent{SocialOp::GuildJoin, c.gid, c.actor, kZeroU64, g.name});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

Result<void> SocialSystem::LeaveGuild(player_id who, guild_id gid) {
    auto git = guilds_.find(gid);
    if (git == guilds_.end()) {
        return Result<void>::Fail(SocErr(ErrorCode::NOT_FOUND, "guild not found"));
    }
    Guild& g = git->second;
    auto f = std::find(g.members.begin(), g.members.end(), who);
    if (f == g.members.end()) {
        return Result<void>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "not a member"));
    }
    const std::string name = g.name;  // 拷贝，避免后续可能 erase 后访问悬空
    g.members.erase(f);
    g.member_count = static_cast<std::uint32_t>(g.members.size());
    player_guild_.erase(who);
    if (g.members.empty()) {
        guilds_.erase(git);
        mmo::data::VersionCheck vc;
        vc.required = false;
        (void)store_.Delete("guild:" + std::to_string(gid), vc);  // 解散：删除持久化记录
    } else {
        if (g.leader == who) g.leader = g.members.front();
        PersistGuild(g);
    }
    (void)bus_.Publish(GuildEvent{SocialOp::Leave, gid, who, kZeroU64, name});
    return Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// Chat（§15.5）
// ---------------------------------------------------------------------------

Result<void> SocialSystem::SendChat(player_id from, ChatChannel channel,
                                    std::string_view message,
                                    player_id to, party_id pid, guild_id gid) {
    if (from == kZeroU64) {
        return Result<void>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid sender"));
    }
    if (message.empty()) {
        return Result<void>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "empty message"));
    }
    ChatEvent ev;
    ev.channel = channel;
    ev.from = from;
    ev.to = to;
    ev.gid = gid;
    ev.pid = pid;
    ev.message = std::string(message);

    switch (channel) {
        case ChatChannel::Private:
            if (to == kZeroU64) {
                return Result<void>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "private needs target"));
            }
            break;
        case ChatChannel::Party:
            if (pid == kZeroU64) {
                return Result<void>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "party chat needs pid"));
            }
            break;
        case ChatChannel::Guild:
            if (gid == kZeroU64) {
                return Result<void>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "guild chat needs gid"));
            }
            break;
        case ChatChannel::World:
            if (!cfg_.world_chat_enabled) {
                return Result<void>::Fail(SocErr(ErrorCode::UNAUTHORIZED, "world chat disabled"));
            }
            // 仅发布单条事件；接收者路由由 Gateway 订阅表完成，绝不做全服 O(N) 遍历。
            break;
    }
    return bus_.Publish(ev);
}

// ---------------------------------------------------------------------------
// Mail（§15.6，幂等领取）
// ---------------------------------------------------------------------------

Result<mail_id> SocialSystem::SendMail(player_id from, player_id to,
                                       std::string_view subject, std::string_view body,
                                       bool has_attachment, std::uint64_t attachment_amount) {
    SocialCommand c;
    c.op = SocialOp::SendMail;
    c.actor = from;
    c.target = to;
    c.subject = std::string(subject);
    c.payload = std::string(body);
    c.has_attachment = has_attachment;
    c.amount = attachment_amount;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<mail_id>::Fail(out.Err());
    return Result<mail_id>::Ok(out.Value().as_mail);
}

Result<SocialOutcome> SocialSystem::HSendMail(const SocialCommand& c) {
    if (c.actor == kZeroU64 || c.target == kZeroU64 || c.actor == c.target) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "invalid mail pair"));
    }
    const mail_id mid = next_mail_id_++;
    Mail m;
    m.id = mid;
    m.from = c.actor;
    m.to = c.target;
    m.subject = c.subject;
    m.body = c.payload;
    m.has_attachment = c.has_attachment;
    m.attachment_amount = c.amount;
    m.claimed = false;
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    m.created_at = now;
    m.expires_at = now + cfg_.mail_ttl_seconds;
    mails_[mid] = std::move(m);
    mailbox_[c.target].push_back(mid);
    PersistMail(mails_[mid]);
    SocialOutcome o;
    o.has_mail = true;
    o.as_mail = mid;
    (void)bus_.Publish(MailEvent{SocialOp::SendMail, mid, c.actor, c.target});
    return Result<SocialOutcome>::Ok(o);
}

Result<void> SocialSystem::ClaimMail(player_id who, mail_id mid) {
    SocialCommand c;
    c.op = SocialOp::ClaimMail;
    c.actor = who;
    c.mid = mid;
    auto out = Dispatch(c);
    if (!out.HasValue()) return Result<void>::Fail(out.Err());
    return Result<void>::Ok();
}

Result<SocialOutcome> SocialSystem::HClaimMail(const SocialCommand& c) {
    auto mit = mails_.find(c.mid);
    if (mit == mails_.end()) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::NOT_FOUND, "mail not found"));
    }
    Mail& m = mit->second;
    if (m.to != c.actor) {
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::UNAUTHORIZED, "not recipient"));
    }
    if (m.claimed) {
        // 幂等：重复领取只发一次附件（§17 / §19）。第二次返回 AlreadyClaimed。
        return Result<SocialOutcome>::Fail(SocErr(ErrorCode::INVALID_ARGUMENT, "mail already claimed"));
    }
    m.claimed = true;
    PersistMail(m);
    (void)bus_.Publish(MailEvent{SocialOp::ClaimMail, m.id, m.from, m.to});
    return Result<SocialOutcome>::Ok(SocialOutcome{});
}

// ---------------------------------------------------------------------------
// 只读查询
// ---------------------------------------------------------------------------

Party* SocialSystem::FindParty(party_id pid) noexcept {
    auto it = parties_.find(pid);
    return it == parties_.end() ? nullptr : &it->second;
}

Guild* SocialSystem::FindGuild(guild_id gid) noexcept {
    auto it = guilds_.find(gid);
    return it == guilds_.end() ? nullptr : &it->second;
}

const std::set<player_id>* SocialSystem::FriendsOf(player_id p) const noexcept {
    auto it = friends_.find(p);
    return it == friends_.end() ? nullptr : &it->second;
}

bool SocialSystem::IsFriend(player_id a, player_id b) const noexcept {
    auto it = friends_.find(a);
    if (it == friends_.end()) return false;
    return it->second.find(b) != it->second.end();
}

std::vector<mail_id> SocialSystem::MailboxOf(player_id p) const {
    auto it = mailbox_.find(p);
    if (it == mailbox_.end()) return {};
    return it->second;
}

// ---------------------------------------------------------------------------
// 持久化辅助
// ---------------------------------------------------------------------------

void SocialSystem::PersistGuild(const Guild& g) {
    mmo::data::Record rec;
    rec.key = "guild:" + std::to_string(g.id);
    rec.payload = SerializeGuild(g);
    mmo::data::VersionCheck vc;
    vc.required = false;  // 经 IDataStore 作简单 KV；版本由上层 DataService 管理
    auto r = store_.Save(rec, vc);
    if (!r.HasValue()) {
        ++pending_guild_saves_;  // §19：持久化失败 → 标记待重试，内存态保留
    }
}

void SocialSystem::PersistMail(const Mail& m) {
    mmo::data::Record rec;
    rec.key = "mail:" + std::to_string(m.id);
    rec.payload = SerializeMail(m);
    mmo::data::VersionCheck vc;
    vc.required = false;
    (void)store_.Save(rec, vc);  // 邮件持久化失败不阻断内存态（最佳努力）
}

void SocialSystem::PersistFriends(player_id p) {
    auto it = friends_.find(p);
    if (it == friends_.end()) return;
    mmo::data::Record rec;
    rec.key = "friend:" + std::to_string(p);
    rec.payload = SerializeFriends(it->second);
    mmo::data::VersionCheck vc;
    vc.required = false;
    (void)store_.Save(rec, vc);
}

}  // namespace mmo::game::social
