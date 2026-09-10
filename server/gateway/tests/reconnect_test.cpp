// server/gateway/tests/reconnect_test.cpp — TASK-037 §16 单元 / §17 集成（37.2 ReconnectService）
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf（红线）。

#include <chrono>
#include <cstdint>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_manager.h"
#include "mmo/gateway/session/session_store.h"
#include "mmo/gateway/resilience/reconnect_service.h"

namespace {

using namespace mmo::gateway;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

namespace core = mmo::core;
namespace net  = mmo::net;
using core::ErrorCode;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

// 合法签名 = player_id ^ nonce（与 session_test 一致，仅供鉴权）。
class TestAuth final : public IAuthProvider {
public:
    core::Result<PlayerId> Authenticate(const AuthToken& token) noexcept override {
        if (token.player_id == kInvalidPlayerId) {
            return core::Result<PlayerId>::Fail(
                core::Error{ErrorCode::UNAUTHORIZED, "empty player", core::domain::kNet});
        }
        if (token.signature != (token.player_id ^ token.nonce)) {
            return core::Result<PlayerId>::Fail(
                core::Error{ErrorCode::UNAUTHORIZED, "bad signature", core::domain::kNet});
        }
        return core::Result<PlayerId>::Ok(token.player_id);
    }
};

AuthToken MakeToken(PlayerId player, std::uint64_t nonce) noexcept {
    return AuthToken{player, nonce, player ^ nonce};
}

core::Result<SessionId> MakeSuspendedSession(SessionManager& mgr, PlayerId player,
                                             net::ConnectionId conn) {
    auto id = mgr.OnConnected(conn);
    if (!id.HasValue()) return id;
    const auto authed = mgr.OnAuthenticate(id.Value(), player, MakeToken(player, 7));
    if (!authed.HasValue()) return core::Result<SessionId>::Fail(authed.Err());
    const auto disc = mgr.OnDisconnected(id.Value(), net::CloseReason::PeerClosed);
    if (!disc.HasValue()) return core::Result<SessionId>::Fail(disc.Err());
    return id;
}

// ---- 注入接口 stub ----

struct StubInfo : ISessionInfoProvider {
    PlayerId     player_id{kInvalidPlayerId};
    std::uint32_t version{0};
    SceneId      scene_id{kInvalidSceneId};
    NodeId       game_node_id{0};
    core::Result<SessionInfo> Lookup(SessionId) const noexcept override {
        SessionInfo i{};
        i.player_id     = player_id;
        i.version       = version;
        i.scene_id      = scene_id;
        i.game_node_id  = game_node_id;
        return core::Result<SessionInfo>::Ok(i);
    }
};

struct StubLoader : IPlayerDataLoader {
    bool fail{false};
    core::Result<void> Load(PlayerId) noexcept override {
        if (fail) {
            return core::Result<void>::Fail(
                core::Error{ErrorCode::NOT_FOUND, "player snapshot missing", core::domain::kNet});
        }
        return core::Result<void>::Ok();
    }
};

struct StubAttacher : ISceneAttacher {
    bool    fail{false};
    NodeId  assigned{7};
    core::Result<NodeId> Attach(PlayerId, SceneId) noexcept override {
        if (fail) {
            return core::Result<NodeId>::Fail(
                core::Error{ErrorCode::BUSY, "scene node busy", core::domain::kNet});
        }
        return core::Result<NodeId>::Ok(assigned);
    }
};

// ---------------------------------------------------------------------------
// §16 单元：六步 happy path
// ---------------------------------------------------------------------------

void TestSixStepHappyPath() {
    core::EventBus         bus;
    InMemorySessionStore   store;
    TestAuth               auth;
    SessionManager         mgr(store, auth, &bus);

    const SessionId sid = MakeSuspendedSession(mgr, 1001, /*conn=*/1).Value();

    StubInfo    info;
    info.player_id = 1001; info.version = 0; info.scene_id = 42; info.game_node_id = 1;
    StubLoader   loader;
    StubAttacher attacher;
    ReconnectService svc(mgr, info, loader, attacher, &bus);

    auto b = svc.Begin(sid, /*new_conn=*/2, /*trace=*/1);
    CHECK(b.HasValue() && b.Value() == ReconnectStep::Disconnected, "Begin -> Disconnected");

    auto s1 = svc.Advance(sid, 2);
    CHECK(s1.HasValue() && s1.Value() == ReconnectStep::Reconnecting, "Advance -> Reconnecting");
    auto s2 = svc.Advance(sid, 3);
    CHECK(s2.HasValue() && s2.Value() == ReconnectStep::Authenticating, "Advance -> Authenticating");
    auto s3 = svc.Advance(sid, 4);
    CHECK(s3.HasValue() && s3.Value() == ReconnectStep::LoadingPlayer, "Advance -> LoadingPlayer");
    auto s4 = svc.Advance(sid, 5);
    CHECK(s4.HasValue() && s4.Value() == ReconnectStep::AttachingScene, "Advance -> AttachingScene");
    auto s5 = svc.Advance(sid, 6);
    CHECK(s5.HasValue() && s5.Value() == ReconnectStep::Resumed, "Advance -> Resumed");

    // 终态不再推进
    auto s6 = svc.Advance(sid, 7);
    CHECK(s6.HasValue() && s6.Value() == ReconnectStep::Resumed, "terminal Resumed stays");

    CHECK(svc.ResumedCount() == 1, "one resumed");
    CHECK(store.Get(sid).Value()->version == 1, "session version bumped to 1 after reattach");

    CHECK(svc.OnComplete(sid).HasValue(), "OnComplete ok");
    CHECK(svc.StatusOf(sid) == ReconnectStep::Disconnected, "state cleared after OnComplete");
    CHECK(svc.ActiveCount() == 0, "no active reconnects");
}

// ---------------------------------------------------------------------------
// §16 单元：version 校验拒绝旧连接（防回放）
// ---------------------------------------------------------------------------

void TestVersionConflict() {
    core::EventBus         bus;
    InMemorySessionStore   store;
    TestAuth               auth;
    SessionManager         mgr(store, auth, &bus);
    const SessionId sid = MakeSuspendedSession(mgr, 2001, /*conn=*/1).Value();

    StubInfo    info;
    info.player_id = 2001; info.version = 99;  // 故意错误 version
    info.scene_id = 42; info.game_node_id = 1;
    StubLoader   loader;
    StubAttacher attacher;
    ReconnectService svc(mgr, info, loader, attacher, &bus);

    CHECK(svc.Begin(sid, /*new_conn=*/2, 1).HasValue(), "begin ok");
    (void)svc.Advance(sid, 2);  // Disconnected -> Reconnecting (rest)
    auto r = svc.Advance(sid, 3);  // Reconnecting 步用 version=99 -> Reattach -> VERSION_CONFLICT
    CHECK(r.HasValue() && r.Value() == ReconnectStep::Failed, "wrong version -> Failed");
    CHECK(svc.StatusOf(sid) == ReconnectStep::Failed, "state is Failed");
    CHECK(svc.FailedCount() == 1, "one failed");
    CHECK(mgr.RejectedReattachCount() == 1, "SessionManager counted 1 rejected reattach");
}

// ---------------------------------------------------------------------------
// §16 单元：Load Player 失败 -> Failed
// ---------------------------------------------------------------------------

void TestLoadPlayerFailure() {
    core::EventBus         bus;
    InMemorySessionStore   store;
    TestAuth               auth;
    SessionManager         mgr(store, auth, &bus);
    const SessionId sid = MakeSuspendedSession(mgr, 3001, 1).Value();

    StubInfo    info;
    info.player_id = 3001; info.version = 0; info.scene_id = 42; info.game_node_id = 1;
    StubLoader   loader; loader.fail = true;  // 玩家快照缺失
    StubAttacher attacher;
    ReconnectService svc(mgr, info, loader, attacher, &bus);

    CHECK(svc.Begin(sid, 2, 1).HasValue(), "begin ok");
    (void)svc.Advance(sid, 2);  // Disconnected -> Reconnecting (rest)
    (void)svc.Advance(sid, 3);  // Reconnecting -> Authenticating (Reattach ok)
    auto r = svc.Advance(sid, 4);  // Authenticating -> LoadingPlayer -> Load fails
    CHECK(r.HasValue() && r.Value() == ReconnectStep::Failed, "load failure -> Failed");
}

// ---------------------------------------------------------------------------
// §16 单元：Attach Scene 失败 -> Failed
// ---------------------------------------------------------------------------

void TestSceneAttachFailure() {
    core::EventBus         bus;
    InMemorySessionStore   store;
    TestAuth               auth;
    SessionManager         mgr(store, auth, &bus);
    const SessionId sid = MakeSuspendedSession(mgr, 4001, 1).Value();

    StubInfo    info;
    info.player_id = 4001; info.version = 0; info.scene_id = 42; info.game_node_id = 1;
    StubLoader   loader;
    StubAttacher attacher; attacher.fail = true;  // 场景节点忙
    ReconnectService svc(mgr, info, loader, attacher, &bus);

    CHECK(svc.Begin(sid, 2, 1).HasValue(), "begin ok");
    (void)svc.Advance(sid, 2);  // Disconnected -> Reconnecting (rest)
    (void)svc.Advance(sid, 3);  // Reconnecting -> Authenticating (Reattach ok)
    (void)svc.Advance(sid, 4);  // Authenticating -> LoadingPlayer (Load ok)
    auto r = svc.Advance(sid, 5);  // LoadingPlayer -> AttachingScene -> Attach fails
    CHECK(r.HasValue() && r.Value() == ReconnectStep::Failed, "attach failure -> Failed");
}

}  // namespace

int main() {
    Line("== TASK-037 gateway resilience (37.2 ReconnectService) test ==\n");
    TestSixStepHappyPath();
    TestVersionConflict();
    TestLoadPlayerFailure();
    TestSceneAttachFailure();
    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
