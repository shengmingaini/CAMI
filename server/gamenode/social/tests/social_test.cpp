// server/gamenode/social/tests/social_test.cpp — TASK-039 单元 / 集成 / 失败测试
//
// 输出走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// 测试序列：组队生命周期 / 好友双向 / 公会双写一致 / 公会持久化失败 / 聊天四频道 /
// 邮件幂等领取。

#include <cstdint>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/data/fake_data_store.h"
#include "mmo/data/in_memory_store.h"
#include "mmo/game/social/social_events.h"
#include "social_io.h"
#include "mmo/game/social/social_system.h"

namespace {

using namespace mmo::game::social;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
namespace core = mmo::core;
using core::ErrorCode;

int g_fails = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

core::EventBus& Bus() {
    static core::EventBus b;
    return b;
}

struct Counters {
    int party = 0;
    int guild = 0;
    int chat = 0;
    int mail = 0;
};
Counters& Cnt() {
    static Counters c;
    return c;
}

void SubscribeAll() {
    (void)Bus().Subscribe<PartyEvent>([](const PartyEvent&) { Cnt().party++; });
    (void)Bus().Subscribe<GuildEvent>([](const GuildEvent&) { Cnt().guild++; });
    (void)Bus().Subscribe<ChatEvent>([](const ChatEvent&) { Cnt().chat++; });
    (void)Bus().Subscribe<MailEvent>([](const MailEvent&) { Cnt().mail++; });
}

void DrainBus() { (void)Bus().Drain(8192, core::DurationMs{1000}); }
void ResetCnt() { Cnt() = Counters{}; }

// ---------------------------------------------------------------------------
void TestPartyLifecycle() {
    ResetCnt();
    mmo::data::InMemoryStore store;
    SocialSystem sys(Bus(), store);

    auto pr = sys.CreateParty(1ULL);
    CHECK(pr.HasValue(), "create party");
    const party_id pid = pr.Value();
    CHECK(sys.FindParty(pid) != nullptr, "party exists");
    CHECK(sys.FindParty(pid)->leader == 1ULL, "leader is 1");
    CHECK(sys.FindParty(pid)->members.size() == 1u, "one member initially");

    CHECK(sys.Invite(1ULL, 2ULL, pid).HasValue(), "invite 2");
    CHECK(sys.AcceptInvite(2ULL, pid).HasValue(), "accept invite");
    CHECK(sys.FindParty(pid)->members.size() == 2u, "two members after accept");

    CHECK(sys.Promote(1ULL, 2ULL, pid).HasValue(), "promote 2 to leader");
    CHECK(sys.FindParty(pid)->leader == 2ULL, "leader now 2");

    CHECK(sys.Kick(2ULL, 1ULL, pid).HasValue(), "kick 1");
    CHECK(sys.FindParty(pid)->members.size() == 1u, "one member after kick");

    CHECK(sys.Leave(2ULL).HasValue(), "leader leaves");
    CHECK(sys.FindParty(pid) == nullptr, "party disbanded when empty");

    // 拒绝路径
    auto pr2 = sys.CreateParty(10ULL);
    const party_id pid2 = pr2.Value();
    CHECK(sys.Invite(10ULL, 11ULL, pid2).HasValue(), "invite 11");
    CHECK(sys.DeclineInvite(11ULL, pid2).HasValue(), "decline invite");
    CHECK(sys.FindParty(pid2)->members.size() == 1u, "decline did not add member");
    auto dup = sys.Invite(10ULL, 11ULL, pid2);
    CHECK(dup.HasValue(), "re-invite after decline ok");
    auto dup2 = sys.Invite(10ULL, 11ULL, pid2);
    CHECK(!dup2.HasValue(), "duplicate active invite rejected");

    DrainBus();
    CHECK(Cnt().party >= 6, "party events emitted");
}

// ---------------------------------------------------------------------------
void TestFriendBidirectional() {
    ResetCnt();
    mmo::data::InMemoryStore store;
    SocialSystem sys(Bus(), store);

    CHECK(sys.AddFriend(100ULL, 200ULL).HasValue(), "add friend");
    CHECK(sys.IsFriend(100ULL, 200ULL), "100 knows 200");
    CHECK(sys.IsFriend(200ULL, 100ULL), "200 knows 100 (bidirectional)");

    auto fr = store.Load("friend:100");
    CHECK(fr.HasValue() && fr.Value().has_value(), "friend:100 persisted to store");

    CHECK(sys.RemoveFriend(100ULL, 200ULL).HasValue(), "remove friend");
    CHECK(!sys.IsFriend(100ULL, 200ULL), "no longer friends");

    DrainBus();
    CHECK(Cnt().party == 0 && Cnt().guild == 0, "friend ops emit no party/guild events");
}

// ---------------------------------------------------------------------------
void TestGuildDoubleWrite() {
    ResetCnt();
    mmo::data::InMemoryStore store;
    SocialSystem sys(Bus(), store);

    auto gr = sys.CreateGuild(5ULL, "Alpha");
    CHECK(gr.HasValue(), "create guild");
    const guild_id gid = gr.Value();
    Guild* g = sys.FindGuild(gid);
    CHECK(g != nullptr, "guild exists");
    CHECK(g->name == "Alpha", "guild name");
    CHECK(g->leader == 5ULL, "guild leader");
    CHECK(g->member_count == 1u, "guild member_count 1");

    // 持久化往返（内存态 == 存储快照）
    auto rec = store.Load("guild:" + std::to_string(gid));
    CHECK(rec.HasValue() && rec.Value().has_value(), "guild persisted to store");
    auto des = DeserializeGuild(rec.Value()->payload);
    CHECK(des.HasValue(), "deserialize guild");
    CHECK(des.Value().name == "Alpha", "persisted name matches");
    CHECK(des.Value().leader == 5ULL, "persisted leader matches");
    CHECK(des.Value().members.size() == 1u, "persisted members match");

    CHECK(sys.JoinGuild(6ULL, gid).HasValue(), "join guild");
    CHECK(sys.FindGuild(gid)->member_count == 2u, "member_count 2 after join");
    CHECK(sys.FindGuild(gid)->members.size() == 2u, "members size 2 after join");

    auto rec2 = store.Load("guild:" + std::to_string(gid));
    auto des2 = DeserializeGuild(rec2.Value()->payload);
    CHECK(des2.Value().members.size() == 2u, "persisted members updated after join");

    CHECK(sys.LeaveGuild(6ULL, gid).HasValue(), "leave guild");
    CHECK(sys.FindGuild(gid)->member_count == 1u, "member_count back to 1");

    DrainBus();
    CHECK(Cnt().guild >= 3, "guild events emitted");
}

// ---------------------------------------------------------------------------
void TestGuildPersistFailure() {
    ResetCnt();
    mmo::data::FakeDataStore store;
    store.set_force_conflict(true);  // 注入持久化失败
    SocialSystem sys(Bus(), store);

    auto gr = sys.CreateGuild(7ULL, "Beta");
    CHECK(gr.HasValue(), "create guild ok despite persist failure");
    const guild_id gid = gr.Value();
    CHECK(sys.FindGuild(gid) != nullptr, "guild retained in memory (no data loss)");
    CHECK(sys.PendingRetries() > 0u, "pending retry marked (§19)");

    DrainBus();
}

// ---------------------------------------------------------------------------
void TestChat() {
    ResetCnt();
    mmo::data::InMemoryStore store;
    SocialSystem sys(Bus(), store);

    CHECK(sys.SendChat(1ULL, ChatChannel::Private, "hi", 2ULL).HasValue(), "private chat");
    CHECK(sys.SendChat(1ULL, ChatChannel::Party, "p", 0ULL, 99ULL).HasValue(), "party chat");
    CHECK(sys.SendChat(1ULL, ChatChannel::Guild, "g", 0ULL, 0ULL, 7ULL).HasValue(), "guild chat");
    CHECK(sys.SendChat(1ULL, ChatChannel::World, "world").HasValue(), "world chat");

    // 非法频道参数
    CHECK(!sys.SendChat(1ULL, ChatChannel::Private, "x", 0ULL).HasValue(), "private needs target");

    DrainBus();
    // 四次合法聊天各发一条事件；世界频道单条发布（无全服 O(N) 遍历，代码评审确认）
    CHECK(Cnt().chat == 4, "exactly 4 chat events (world chat = single publish)");

    // 世界频道关闭时拒绝
    SocialConfig cfg;
    cfg.world_chat_enabled = false;
    SocialSystem sys2(Bus(), store, cfg);
    CHECK(!sys2.SendChat(1ULL, ChatChannel::World, "no").HasValue(), "world disabled rejected");
}

// ---------------------------------------------------------------------------
void TestMailIdempotent() {
    ResetCnt();
    mmo::data::InMemoryStore store;
    SocialSystem sys(Bus(), store);

    auto mr = sys.SendMail(1ULL, 2ULL, "subj", "body", true, 500ULL);
    CHECK(mr.HasValue(), "send mail");
    const mail_id mid = mr.Value();
    CHECK(sys.MailboxOf(2ULL).size() == 1u, "mailbox has 1");

    auto rec = store.Load("mail:" + std::to_string(mid));
    CHECK(rec.HasValue() && rec.Value().has_value(), "mail persisted to store");

    CHECK(sys.ClaimMail(2ULL, mid).HasValue(), "first claim ok");
    auto second = sys.ClaimMail(2ULL, mid);
    CHECK(!second.HasValue(), "second claim rejected (idempotent)");
    if (!second.HasValue()) {
        CHECK(second.Err().Code() == ErrorCode::INVALID_ARGUMENT, "already-claimed code");
        CHECK(std::string(second.Err().Message()) == "mail already claimed", "already-claimed msg");
    }

    auto bad = sys.ClaimMail(3ULL, mid);
    CHECK(!bad.HasValue(), "non-recipient claim rejected");
    if (!bad.HasValue()) {
        CHECK(bad.Err().Code() == ErrorCode::UNAUTHORIZED, "unauthorized code");
    }

    auto missing = sys.ClaimMail(2ULL, 99999ULL);
    CHECK(!missing.HasValue(), "claim missing mail rejected");

    DrainBus();
    CHECK(Cnt().mail >= 2, "mail events emitted");
}

}  // namespace

int main() {
    Line("== TASK-039 social system test ==\n");
    SubscribeAll();
    TestPartyLifecycle();
    TestFriendBidirectional();
    TestGuildDoubleWrite();
    TestGuildPersistFailure();
    TestChat();
    TestMailIdempotent();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
