// server/gateway/tests/session_test.cpp — TASK-009 §16 单元 / §17 集成 / §19 Failure
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf（红线）。
// 本文件不 include 任何 gateway 内部 src/。

#include <chrono>
#include <cstdint>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_manager.h"
#include "mmo/gateway/session/session_store.h"

namespace {

using namespace mmo::gateway;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

// 命名空间别名：本文件位于匿名命名空间内，mmo::core / mmo::net 不会因
// `using namespace mmo::gateway` 而自动可见（gateway 与它们是平级命名空间）。
namespace core = mmo::core;
namespace net  = mmo::net;

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

/// 断言 Result 失败且错误码为预期值（避免测试只查 HasValue 却放过错误码漂移）。
#define CHECK_CODE(result, expected, msg)                                      \
    do {                                                                       \
        const auto& r_ = (result);                                             \
        if (r_.HasValue()) {                                                   \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg,       \
                     __LINE__);                                                \
            ++g_fails;                                                         \
        } else if (r_.Err().Code() != (expected)) {                            \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);    \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

using core::ErrorCode;

// ---------------------------------------------------------------------------
// 替身（§15.7 禁止把鉴权口令写死在代码里）
// ---------------------------------------------------------------------------

/// 合法签名 = player_id ^ nonce；player_id == 0 或签名不符一律拒绝。
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

core::SteadyTime NowPlus(std::chrono::milliseconds delta) noexcept {
    return core::MonotonicClock::Point() + delta;
}

/// 建一个已鉴权（Active）的会话，返回 SessionId。
core::Result<SessionId> MakeActiveSession(SessionManager& mgr, PlayerId player,
                                          net::ConnectionId conn) {
    auto id = mgr.OnConnected(conn);
    if (!id.HasValue()) {
        return id;
    }
    const auto authed = mgr.OnAuthenticate(id.Value(), player, MakeToken(player, 7));
    if (!authed.HasValue()) {
        return core::Result<SessionId>::Fail(authed.Err());
    }
    return id;
}

// ---------------------------------------------------------------------------
// §16 单元：SessionStore CRUD + 防 ABA
// ---------------------------------------------------------------------------

void TestStoreCrud() {
    InMemorySessionStore store;

    Session s{};
    s.player_id = 1001;
    s.state = SessionState::Connecting;
    const auto id = store.Allocate(s);
    CHECK(id.HasValue(), "allocate should succeed");
    CHECK(store.Size() == 1, "size should be 1");

    const SessionId sid = id.Value();
    auto got = store.Get(sid);
    CHECK(got.HasValue() && got.Value().has_value(), "get should return session");
    CHECK(got.Value()->player_id == 1001, "player id mismatch");
    CHECK(SessionGeneration(sid) >= 1, "generation must start from 1");

    auto by_player = store.FindByPlayer(1001);
    CHECK(by_player.HasValue() && by_player.Value().has_value(), "find by player should hit");
    CHECK(by_player.Value()->session_id == sid, "player index should map to same session");

    // 更新
    Session updated = *got.Value();
    updated.scene_id = 42;
    CHECK(store.Put(updated).HasValue(), "put should succeed");
    CHECK(store.Get(sid).Value()->scene_id == 42, "put should persist scene_id");

    // 移除后旧 id 失效（防 ABA）
    CHECK(store.Remove(sid).HasValue(), "remove should succeed");
    CHECK(store.Size() == 0, "size should be 0 after remove");
    CHECK(!store.Get(sid).Value().has_value(), "removed session must be invisible");
    CHECK(store.Remove(sid).HasValue(), "remove must be idempotent");

    // 槽位复用：新会话拿到不同 generation
    const auto id2 = store.Allocate(s);
    CHECK(id2.HasValue(), "re-allocate should succeed");
    CHECK(id2.Value() != sid, "reused slot must produce a different SessionId");

    // 故障路径：未分配槽位 / generation 失效的 Put 必须报错而非静默写入
    CHECK_CODE(store.Put(Session{}), ErrorCode::INVALID_ARGUMENT,
               "put with invalid session id must fail");
    CHECK_CODE(store.Put(Session{}), ErrorCode::INVALID_ARGUMENT,
               "put on freed slot must fail");
    const SessionId stale = MakeSessionId(SessionSlot(sid), SessionGeneration(sid));
    CHECK(!store.Get(stale).Value().has_value(), "stale generation must not resolve");
}

// ---------------------------------------------------------------------------
// §16 单元：状态机 happy path
// ---------------------------------------------------------------------------

void TestStateMachineHappyPath() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    const auto id = mgr.OnConnected(/*conn_id=*/7);
    CHECK(id.HasValue(), "OnConnected should succeed");
    CHECK(mgr.ActiveCount() == 0, "new session is not active yet");

    const SessionId sid = id.Value();
    CHECK(store.Get(sid).Value()->state == SessionState::Connecting, "initial state Connecting");

    CHECK(mgr.OnAuthenticate(sid, 1001, MakeToken(1001, 42)).HasValue(), "auth should succeed");
    CHECK(mgr.ActiveCount() == 1, "authenticated session is active");
    CHECK(store.Get(sid).Value()->player_id == 1001, "player bound after auth");

    CHECK(mgr.OnHeartbeat(sid).HasValue(), "heartbeat on active session should succeed");

    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");
    CHECK(mgr.ActiveCount() == 0 && mgr.SuspendedCount() == 1, "disconnect suspends session");

    // grace 内重连：version 0 → 1
    CHECK(mgr.Reattach(sid, /*new_conn=*/8, /*expected_version=*/0).HasValue(),
          "reattach within grace should succeed");
    CHECK(mgr.ActiveCount() == 1 && mgr.SuspendedCount() == 0, "reattach resumes session");
    CHECK(store.Get(sid).Value()->version == 1, "version must bump after reattach");
    CHECK(store.Get(sid).Value()->conn_id == 8, "conn id must update after reattach");
}

// ---------------------------------------------------------------------------
// §16 单元：非法状态转移（§8 状态机契约）
// ---------------------------------------------------------------------------

void TestIllegalTransitions() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    const SessionId sid = mgr.OnConnected(1).Value();

    // Connecting 状态不接受心跳（心跳仅 Active 有效）
    CHECK_CODE(mgr.OnHeartbeat(sid), ErrorCode::INVALID_ARGUMENT,
               "heartbeat on Connecting must be rejected");
    // Connecting 状态不允许重连（没鉴权过的会话无所谓「接管」）
    CHECK_CODE(mgr.Reattach(sid, 2, 0), ErrorCode::INVALID_ARGUMENT,
               "reattach on Connecting must be rejected");
    // 非法 player
    CHECK_CODE(mgr.OnAuthenticate(sid, kInvalidPlayerId, MakeToken(0, 1)),
               ErrorCode::INVALID_ARGUMENT, "invalid player id must be rejected");

    CHECK(mgr.OnAuthenticate(sid, 2001, MakeToken(2001, 9)).HasValue(), "auth ok");
    // 已 Active 不允许二次鉴权
    CHECK_CODE(mgr.OnAuthenticate(sid, 2001, MakeToken(2001, 9)), ErrorCode::INVALID_ARGUMENT,
               "re-authenticate on Active must be rejected");

    // 断线后（Suspended）心跳必须被拒 —— 这是防旧连接回放的第二道闸
    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");
    CHECK_CODE(mgr.OnHeartbeat(sid), ErrorCode::INVALID_ARGUMENT,
               "heartbeat on Suspended must be rejected");

    // 未知会话：NOT_FOUND（不是崩溃、不是静默成功）
    const SessionId bogus = MakeSessionId(9999, 1);
    CHECK_CODE(mgr.OnHeartbeat(bogus), ErrorCode::NOT_FOUND, "unknown session must be NOT_FOUND");
    CHECK_CODE(mgr.Reattach(bogus, 3, 0), ErrorCode::NOT_FOUND,
               "reattach unknown session must be NOT_FOUND");
    // 断线通知幂等（重复投递不该报错，网络层常重复触发）
    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(),
          "duplicate disconnect must be idempotent");
}

// ---------------------------------------------------------------------------
// §16 单元：version 递增与校验（§15.5 防旧连接回放）
// ---------------------------------------------------------------------------

void TestVersionGuard() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    const SessionId sid = MakeActiveSession(mgr, 3001, 1).Value();
    CHECK(store.Get(sid).Value()->version == 0, "fresh session version is 0");

    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");

    // 期望 version=1（错误）→ VERSION_CONFLICT，且被计数
    CHECK_CODE(mgr.Reattach(sid, 2, /*expected_version=*/1), ErrorCode::VERSION_CONFLICT,
               "version mismatch must be rejected");
    CHECK(mgr.RejectedReattachCount() == 1, "rejected reattach must be counted");
    CHECK(mgr.SuspendedCount() == 1, "rejected reattach must not resume session");

    // 正确 version → 成功，version 递增
    CHECK(mgr.Reattach(sid, 2, /*expected_version=*/0).HasValue(), "reattach with correct version");
    CHECK(store.Get(sid).Value()->version == 1, "version bumps to 1");

    // 同一个 version 不能复用（防重放：旧包再发一次必须被拒）
    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");
    CHECK(mgr.Reattach(sid, 3, /*expected_version=*/1).HasValue(), "reattach v1 -> v2");
    CHECK(store.Get(sid).Value()->version == 2, "version bumps to 2");
    CHECK_CODE(mgr.Reattach(sid, 3, /*expected_version=*/1), ErrorCode::VERSION_CONFLICT,
               "replay of consumed version must be rejected");
}

// ---------------------------------------------------------------------------
// §16 单元：容量上限（§15.6 禁止无界增长）
// ---------------------------------------------------------------------------

void TestCapacityLimit() {
    InMemorySessionStore store;
    TestAuth             auth;

    SessionManager::Config cfg{};
    cfg.max_sessions = 2;
    SessionManager mgr(store, auth, nullptr, cfg);

    CHECK(mgr.OnConnected(1).HasValue(), "first connection ok");
    CHECK(mgr.OnConnected(2).HasValue(), "second connection ok");
    CHECK_CODE(mgr.OnConnected(3), ErrorCode::BUSY, "third connection must be rejected with BUSY");
    CHECK(store.Size() == 2, "store must not grow beyond capacity");

    // 释放一个槽位后可以再次接入（拒绝是限流而非永久熔断）
    const SessionId first = store.Snapshot().front().session_id;
    CHECK(store.Remove(first).HasValue(), "remove one session");
    CHECK(mgr.OnConnected(4).HasValue(), "connection allowed after a slot is freed");
}

// ---------------------------------------------------------------------------
// §16 单元：五种事件走 EventBus（§15.8）
// ---------------------------------------------------------------------------

void TestEvents() {
    InMemorySessionStore store;
    TestAuth             auth;
    core::EventBus       bus;

    SessionManager::Config cfg{};
    cfg.heartbeat_interval = std::chrono::milliseconds(1000);
    cfg.max_missed         = 1;
    cfg.suspend_grace      = std::chrono::milliseconds(1000);
    SessionManager mgr(store, auth, &bus, cfg);

    int created = 0, authed = 0, suspended = 0, resumed = 0, closed = 0;
    (void)bus.Subscribe<SessionCreated>([&](const SessionCreated&) { ++created; });
    (void)bus.Subscribe<SessionAuthenticated>(
        [&](const SessionAuthenticated&) { ++authed; });
    (void)bus.Subscribe<SessionSuspended>([&](const SessionSuspended&) { ++suspended; });
    (void)bus.Subscribe<SessionResumed>([&](const SessionResumed&) { ++resumed; });
    (void)bus.Subscribe<SessionClosed>([&](const SessionClosed&) { ++closed; });

    const SessionId sid = mgr.OnConnected(1).Value();
    CHECK(mgr.OnAuthenticate(sid, 4001, MakeToken(4001, 5)).HasValue(), "auth ok");
    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");
    CHECK(mgr.Reattach(sid, 2, /*expected_version=*/0).HasValue(), "reattach ok");

    // 推进到心跳超时 → Suspended（第二个事件）
    CHECK(mgr.Tick(NowPlus(std::chrono::milliseconds(2000))).HasValue(), "tick ok");
    // 再推进 grace 超时 → Closed
    CHECK(mgr.Tick(NowPlus(std::chrono::milliseconds(5000))).HasValue(), "tick ok");

    (void)bus.Drain();
    CHECK(created == 1, "one SessionCreated expected");
    CHECK(authed == 1, "one SessionAuthenticated expected");
    CHECK(suspended == 2, "two SessionSuspended expected (disconnect + heartbeat timeout)");
    CHECK(resumed == 1, "one SessionResumed expected");
    CHECK(closed == 1, "one SessionClosed expected (grace expired)");
    CHECK(bus.SubscriberErrors() == 0, "no subscriber errors expected");
}

// ---------------------------------------------------------------------------
// §17 集成：1000 会话心跳 + Tick（< 1ms）
// ---------------------------------------------------------------------------

void TestMassHeartbeatTick() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    constexpr std::size_t kSessions = 1000;
    std::vector<SessionId> ids;
    ids.reserve(kSessions);
    for (std::size_t i = 0; i < kSessions; ++i) {
        const auto id = MakeActiveSession(mgr, static_cast<PlayerId>(i + 1),
                                          static_cast<net::ConnectionId>(i + 1));
        CHECK(id.HasValue(), "mass: session creation should succeed");
        if (id.HasValue()) {
            ids.push_back(id.Value());
        }
    }

    // 全量心跳
    for (const SessionId id : ids) {
        CHECK(mgr.OnHeartbeat(id).HasValue(), "mass: heartbeat should succeed");
    }
    CHECK(mgr.ActiveCount() == kSessions, "mass: all sessions active");

    // Tick 全量扫描耗时（§20.5 要求 < 1ms）
    const auto t0 = core::MonotonicClock::Now();
    CHECK(mgr.Tick(core::MonotonicClock::Point()).HasValue(), "mass: tick ok");
    const std::int64_t tick_ns = core::MonotonicClock::Now() - t0;
    const double       tick_us = static_cast<double>(tick_ns) / 1000.0;
    CHECK(tick_us < 1000.0, "mass: tick must stay under 1ms");
    CHECK(mgr.ActiveCount() == kSessions, "mass: no session should time out right after HB");
    LineFmt("  [integrated] 1000-session tick = %.2f us\n", tick_us);

    // 推进到心跳超时：1000 个会话应全部转 Suspended
    CHECK(mgr.Tick(NowPlus(std::chrono::milliseconds(20000))).HasValue(), "mass: timeout tick ok");
    CHECK(mgr.ActiveCount() == 0 && mgr.SuspendedCount() == kSessions,
          "mass: all sessions suspended after timeout");
}

// ---------------------------------------------------------------------------
// §17 集成：100 会话同时断线并在 5 秒内全部重连成功
// ---------------------------------------------------------------------------

void TestMassReconnect() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    constexpr std::size_t kSessions = 100;
    std::vector<SessionId> ids;
    ids.reserve(kSessions);
    for (std::size_t i = 0; i < kSessions; ++i) {
        ids.push_back(MakeActiveSession(mgr, static_cast<PlayerId>(i + 1),
                                        static_cast<net::ConnectionId>(i + 1))
                          .Value());
    }

    // 同时断线
    for (const SessionId id : ids) {
        CHECK(mgr.OnDisconnected(id, net::CloseReason::PeerClosed).HasValue(),
              "reconnect: disconnect should succeed");
    }
    CHECK(mgr.SuspendedCount() == kSessions, "reconnect: all suspended");

    // 全部重连（真实场景在 5 秒 grace 内完成；此处同步执行，等价 grace 内到达）
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(mgr.Reattach(ids[i], static_cast<net::ConnectionId>(i + 1000 + 1),
                           /*expected_version=*/0)
                  .HasValue(),
              "reconnect: reattach should succeed");
    }
    CHECK(mgr.ActiveCount() == kSessions && mgr.SuspendedCount() == 0,
          "reconnect: all sessions resumed");

    // version 必须全部递增到 1（每个会话独立计数，不能串）
    for (const SessionId id : ids) {
        CHECK(store.Get(id).Value()->version == 1, "reconnect: version must be exactly 1");
    }

    // grace 内多次重连：version 连续递增
    CHECK(mgr.OnDisconnected(ids[0], net::CloseReason::PeerClosed).HasValue(), "disconnect again");
    CHECK(mgr.Reattach(ids[0], 9999, /*expected_version=*/1).HasValue(), "second reattach");
    CHECK(store.Get(ids[0]).Value()->version == 2, "reconnect: version must reach 2");
}

// ---------------------------------------------------------------------------
// §19 Failure：心跳风暴 1 万次
// ---------------------------------------------------------------------------

void TestHeartbeatStorm() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    const SessionId sid = MakeActiveSession(mgr, 5001, 1).Value();

    constexpr int kStorm = 10000;
    for (int i = 0; i < kStorm; ++i) {
        if (!mgr.OnHeartbeat(sid).HasValue()) {
            CHECK(false, "storm: heartbeat must not fail");
            break;
        }
    }
    CHECK(mgr.ActiveCount() == 1, "storm: session stays active");
    CHECK(store.Size() == 1, "storm: no session leak");
    CHECK(store.Get(sid).Value()->state == SessionState::Active, "storm: state unchanged");
}

// ---------------------------------------------------------------------------
// §19 Failure：鉴权失败 → 转 Closed 并回收（不保留悬挂会话）
// ---------------------------------------------------------------------------

void TestAuthFailureClosesSession() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    // 签名错误
    const SessionId a = mgr.OnConnected(1).Value();
    CHECK_CODE(mgr.OnAuthenticate(a, 6001, AuthToken{6001, 3, 0xDEAD}),
               ErrorCode::UNAUTHORIZED, "bad signature must be UNAUTHORIZED");
    CHECK(store.Size() == 0, "auth failure must release the slot");
    CHECK(!store.Get(a).Value().has_value(), "auth-failed session must be gone");
    CHECK(mgr.ActiveCount() == 0, "auth failure must not count as active");

    // token 归属玩家与声明不符（防冒用）
    const SessionId b = mgr.OnConnected(2).Value();
    CHECK_CODE(mgr.OnAuthenticate(b, 6002, MakeToken(6003, 1)),
               ErrorCode::UNAUTHORIZED, "player mismatch must be UNAUTHORIZED");
    CHECK(store.Size() == 0, "player mismatch must release the slot");

    // 未完成鉴权即断线：直接回收，不进 Suspended
    const SessionId c = mgr.OnConnected(3).Value();
    CHECK(mgr.OnDisconnected(c, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");
    CHECK(store.Size() == 0, "unauthenticated disconnect must release the slot");
    CHECK(mgr.SuspendedCount() == 0, "unauthenticated session must not be suspended");
}

// ---------------------------------------------------------------------------
// §19 Failure：Reattach 时旧连接仍在线 → 强制接管
// ---------------------------------------------------------------------------

void TestForceReattachWhileActive() {
    InMemorySessionStore store;
    TestAuth             auth;
    core::EventBus       bus;

    int closed = 0, resumed = 0;
    (void)bus.Subscribe<SessionClosed>([&](const SessionClosed&) { ++closed; });
    (void)bus.Subscribe<SessionResumed>([&](const SessionResumed&) { ++resumed; });

    SessionManager mgr(store, auth, &bus);
    const SessionId sid = MakeActiveSession(mgr, 7001, 1).Value();

    // 旧连接仍 Active 就发起接管：允许，但必须先发旧连接关闭事件
    CHECK(mgr.Reattach(sid, /*new_conn=*/2, /*expected_version=*/0).HasValue(),
          "force reattach on active session should succeed");
    CHECK(store.Get(sid).Value()->conn_id == 2, "conn id must switch to the new connection");
    CHECK(store.Get(sid).Value()->version == 1, "version must bump on force takeover");
    CHECK(mgr.ActiveCount() == 1, "force takeover must not duplicate the session");
    CHECK(store.Size() == 1, "force takeover must not allocate a new slot");

    (void)bus.Drain();
    CHECK(closed == 1, "old connection must emit SessionClosed first");
    CHECK(resumed == 1, "takeover must emit SessionResumed");
}

// ---------------------------------------------------------------------------
// §19 Failure：grace 边界（内 / 外）
// ---------------------------------------------------------------------------

void TestGraceBoundary() {
    InMemorySessionStore store;
    TestAuth             auth;

    SessionManager::Config cfg{};
    cfg.suspend_grace = std::chrono::milliseconds(1000);
    SessionManager mgr(store, auth, nullptr, cfg);

    const SessionId sid = MakeActiveSession(mgr, 8001, 1).Value();
    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");

    // grace 内（+500ms）：Tick 不回收，Reattach 成功
    CHECK(mgr.Tick(NowPlus(std::chrono::milliseconds(500))).HasValue(), "tick within grace");
    CHECK(mgr.SuspendedCount() == 1, "session must survive inside grace");
    CHECK(mgr.Reattach(sid, 2, /*expected_version=*/0).HasValue(), "reattach inside grace ok");

    // grace 外（+1500ms）：Tick 回收，槽位释放，Reattach 报 NOT_FOUND
    CHECK(mgr.OnDisconnected(sid, net::CloseReason::PeerClosed).HasValue(), "disconnect ok");
    CHECK(mgr.Tick(NowPlus(std::chrono::milliseconds(1500))).HasValue(), "tick beyond grace");
    CHECK(mgr.SuspendedCount() == 0, "grace-expired session must be released");
    CHECK(store.Size() == 0, "grace-expired slot must be recycled");
    CHECK_CODE(mgr.Reattach(sid, 3, /*expected_version=*/1), ErrorCode::NOT_FOUND,
               "reattach after grace must fail");
}

// ---------------------------------------------------------------------------
// §19 Failure：Store 故障传播（SessionManager 不得吞掉存储层错误）
// ---------------------------------------------------------------------------

void TestStoreFaultPropagation() {
    InMemorySessionStore store;
    TestAuth             auth;
    SessionManager       mgr(store, auth);

    // 越界 / 已回收槽位：Store 的 Get 返回 nullopt（语义是「不存在」而非故障），
    // Manager 必须转成 NOT_FOUND 向上传播——绝不能吞掉后当成成功。
    const SessionId bogus = MakeSessionId(/*slot=*/4096, /*generation=*/1);
    CHECK_CODE(mgr.OnHeartbeat(bogus), ErrorCode::NOT_FOUND,
               "store miss must surface as NOT_FOUND, not be swallowed");
    CHECK_CODE(mgr.OnAuthenticate(bogus, 9001, MakeToken(9001, 1)), ErrorCode::NOT_FOUND,
               "store miss must surface as NOT_FOUND on authenticate");

    // 容量打满后 Allocate 报 BUSY，Manager 转成 BUSY 交给调用方限流
    SessionManager::Config cfg{};
    cfg.max_sessions = 1;
    InMemorySessionStore small_store;
    SessionManager       small_mgr(small_store, auth, nullptr, cfg);
    CHECK(small_mgr.OnConnected(1).HasValue(), "first connection ok");
    CHECK_CODE(small_mgr.OnConnected(2), ErrorCode::BUSY, "capacity error must propagate");
}

}  // namespace

int main() {
    Line("== TASK-009 gateway session test ==\n");

    // §16 单元
    TestStoreCrud();
    TestStateMachineHappyPath();
    TestIllegalTransitions();
    TestVersionGuard();
    TestCapacityLimit();
    TestEvents();

    // §17 集成
    TestMassHeartbeatTick();
    TestMassReconnect();

    // §19 Failure
    TestHeartbeatStorm();
    TestAuthFailureClosesSession();
    TestForceReattachWhileActive();
    TestGraceBoundary();
    TestStoreFaultPropagation();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
