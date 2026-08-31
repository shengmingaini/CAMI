// server/gateway/src/session/session_manager.cpp — TASK-009 §15.3~8
//
// 六状态机 + 心跳超时 + grace 释放 + Reattach 防回放。
// 状态变更统一走 Load → 修改 → Save（Store 接口返回拷贝，Session 为 64B POD，
// 拷贝成本远低于引入内部指针造成的封装破坏）。

#include "mmo/gateway/session/session_manager.h"

#include <chrono>
#include <vector>

namespace mmo::gateway {
namespace {

using core::ErrorCode;
using core::MonotonicClock;

core::Error MakeError(ErrorCode code, const char* message) {
    return core::Error{code, message, core::domain::kNet};
}

/// MonotonicClock::Elapsed 直接返回纳秒整数（SteadyNs = long long），
/// 不是 std::chrono::duration，不能再做 duration_cast。
std::int64_t ToNanos(core::SteadyTime from, core::SteadyTime to) noexcept {
    return static_cast<std::int64_t>(MonotonicClock::Elapsed(from, to));
}

}  // namespace

// 委托构造：Config{} 出现在类外定义处（类定义已完整），绕开嵌套 NSDMI 默认实参限制。
SessionManager::SessionManager(InMemorySessionStore& store, IAuthProvider& auth,
                               core::EventBus* bus)
    : SessionManager(store, auth, bus, Config{}) {}

SessionManager::SessionManager(InMemorySessionStore& store, IAuthProvider& auth,
                               core::EventBus* bus, Config config)
    : store_(store), auth_(auth), bus_(bus), config_(config) {}

// ---------------------------------------------------------------------------
// 内部辅助
// ---------------------------------------------------------------------------

core::Result<Session> SessionManager::Load(SessionId id) {
    auto got = store_.Get(id);
    if (!got.HasValue()) {
        return core::Result<Session>::Fail(got.Err());
    }
    if (!got.Value().has_value()) {
        return core::Result<Session>::Fail(MakeError(ErrorCode::NOT_FOUND, "session not found"));
    }
    return core::Result<Session>::Ok(*got.Value());
}

core::Result<void> SessionManager::Save(const Session& s) { return store_.Put(s); }

void SessionManager::PublishCreated(const Session& s) {
    if (bus_ == nullptr) {
        return;
    }
    (void)bus_->Publish(SessionCreated{s.session_id, s.conn_id, s.trace_id});
}

void SessionManager::PublishClosed(const Session& s, net::CloseReason reason) {
    if (bus_ == nullptr) {
        return;
    }
    (void)bus_->Publish(SessionClosed{s.session_id, s.player_id, reason, s.version});
}

/// 转 Closed → 发事件 → 从 Store 回收槽位。
core::Result<void> SessionManager::CloseAndRemove(SessionId id, net::CloseReason reason) {
    auto loaded = Load(id);
    if (!loaded.HasValue()) {
        return core::Result<void>::Ok();  // 已不存在：幂等
    }
    Session s = loaded.Value();
    if (s.state == SessionState::Active) {
        --active_count_;
    } else if (s.state == SessionState::Suspended) {
        --suspended_count_;
    }
    s.state = SessionState::Closed;
    PublishClosed(s, reason);
    return store_.Remove(id);
}

// ---------------------------------------------------------------------------
// §7 Public Interface
// ---------------------------------------------------------------------------

core::Result<SessionId> SessionManager::OnConnected(net::ConnectionId conn_id) {
    // §15.6 容量上限：达上限拒绝新连接（禁止无界增长）
    if (store_.Size() >= config_.max_sessions) {
        return core::Result<SessionId>::Fail(
            MakeError(ErrorCode::BUSY, "session capacity reached"));
    }

    Session s{};
    s.conn_id        = conn_id;
    s.state          = SessionState::Connecting;
    s.last_heartbeat = MonotonicClock::Point();

    auto allocated = store_.Allocate(s);
    if (!allocated.HasValue()) {
        return core::Result<SessionId>::Fail(allocated.Err());
    }
    s.session_id = allocated.Value();
    s.trace_id   = s.session_id;  // 第一版：会话 ID 兼作 TraceID（串联日志）
    (void)Save(s);
    PublishCreated(s);
    return core::Result<SessionId>::Ok(s.session_id);
}

core::Result<void> SessionManager::OnAuthenticate(SessionId id, PlayerId player,
                                                  const AuthToken& token) {
    if (player == kInvalidPlayerId) {
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "invalid player id"));
    }
    auto loaded = Load(id);
    if (!loaded.HasValue()) {
        return core::Result<void>::Fail(loaded.Err());
    }
    Session s = loaded.Value();

    // 合法起点：Connecting 或 Authenticating（重试探）；其它状态为非法转移
    if (s.state != SessionState::Connecting && s.state != SessionState::Authenticating) {
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "bad state for authenticate"));
    }

    // 中间态：Authenticating（可观测，便于排查鉴权卡死）
    s.state = SessionState::Authenticating;
    (void)Save(s);

    const auto auth_result = auth_.Authenticate(token);
    if (!auth_result.HasValue()) {
        // §19 鉴权失败：转 Closed 并回收，不保留悬挂会话
        (void)CloseAndRemove(id, net::CloseReason::Rejected);
        return core::Result<void>::Fail(auth_result.Err());
    }
    if (auth_result.Value() != player) {
        // token 归属的玩家与声明不符：视为鉴权失败（防冒用）
        (void)CloseAndRemove(id, net::CloseReason::Rejected);
        return core::Result<void>::Fail(
            MakeError(ErrorCode::UNAUTHORIZED, "token player mismatch"));
    }

    // 鉴权可能触发订阅者回调，重新加载避免覆盖期间的状态变更
    auto reloaded = Load(id);
    if (!reloaded.HasValue()) {
        return core::Result<void>::Fail(reloaded.Err());
    }
    Session live = reloaded.Value();
    if (live.state != SessionState::Authenticating) {
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "state changed during auth"));
    }

    live.player_id      = player;
    live.state          = SessionState::Active;
    live.last_heartbeat = MonotonicClock::Point();
    ++active_count_;
    (void)Save(live);

    if (bus_ != nullptr) {
        (void)bus_->Publish(SessionAuthenticated{live.session_id, live.player_id,
                                                 live.version, live.trace_id});
    }
    return core::Result<void>::Ok();
}

core::Result<void> SessionManager::OnHeartbeat(SessionId id) {
    auto loaded = Load(id);
    if (!loaded.HasValue()) {
        return core::Result<void>::Fail(loaded.Err());
    }
    Session s = loaded.Value();
    if (s.state != SessionState::Active) {
        // 非 Active 会话的心跳视为非法（含已 Suspended 的旧连接回放）
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "heartbeat on non-active session"));
    }
    s.last_heartbeat = MonotonicClock::Point();
    return Save(s);
}

core::Result<void> SessionManager::OnDisconnected(SessionId id, net::CloseReason reason) {
    auto loaded = Load(id);
    if (!loaded.HasValue()) {
        return core::Result<void>::Ok();  // 幂等：重复断线通知不报错
    }
    Session s = loaded.Value();

    switch (s.state) {
        case SessionState::Active:
            // 断线 → 挂起，进入 grace 计时（last_heartbeat 复用为断线时刻起点）
            --active_count_;
            ++suspended_count_;
            s.state          = SessionState::Suspended;
            s.last_heartbeat = MonotonicClock::Point();
            (void)Save(s);
            if (bus_ != nullptr) {
                (void)bus_->Publish(SessionSuspended{s.session_id, s.player_id, reason,
                                                     s.version});
            }
            return core::Result<void>::Ok();

        case SessionState::Connecting:
        case SessionState::Authenticating:
            // 未完成鉴权即断线：直接关闭回收（§19 不保留悬挂会话）
            return CloseAndRemove(id, reason);

        default:
            // Suspended / Closing / Closed 保持现状
            return core::Result<void>::Ok();
    }
}

core::Result<void> SessionManager::Reattach(SessionId id, net::ConnectionId new_conn,
                                            std::uint32_t expected_version) {
    auto loaded = Load(id);
    if (!loaded.HasValue()) {
        return core::Result<void>::Fail(loaded.Err());
    }
    Session s = loaded.Value();

    // §15.5 防旧连接回放：版本必须严格匹配（先于状态检查，避免信息泄露）
    if (s.version != expected_version) {
        ++rejected_reattach_;
        return core::Result<void>::Fail(
            MakeError(ErrorCode::VERSION_CONFLICT, "session version mismatch"));
    }
    if (s.state == SessionState::Closed) {
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "session already closed"));
    }

    if (s.state == SessionState::Suspended) {
        // grace 检查（Tick 通常会先回收，此处为并发重连的兜底判定）
        const std::int64_t grace_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(config_.suspend_grace)
                .count();
        if (ToNanos(s.last_heartbeat, MonotonicClock::Point()) > grace_ns) {
            return core::Result<void>::Fail(
                MakeError(ErrorCode::TIMEOUT, "suspend grace expired"));
        }
        --suspended_count_;
        ++active_count_;
    } else if (s.state == SessionState::Active) {
        // §19 旧连接仍在线：强制接管，先发旧连接关闭事件（事件顺序：Closed → Resumed）
        PublishClosed(s, net::CloseReason::Rejected);
    } else {
        // Connecting / Authenticating / Closing 不允许重连
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "bad state for reattach"));
    }

    const std::uint32_t old_version = s.version;
    s.conn_id        = new_conn;
    s.version        = old_version + 1;  // 每次重连 +1（§8 防旧连接回放）
    s.state          = SessionState::Active;
    s.last_heartbeat = MonotonicClock::Point();
    (void)Save(s);

    if (bus_ != nullptr) {
        (void)bus_->Publish(SessionResumed{s.session_id, s.player_id, s.version, old_version});
    }
    return core::Result<void>::Ok();
}

core::Result<void> SessionManager::Tick(core::SteadyTime now) {
    // 心跳预算 = interval × max_missed；超时转 Suspended
    const std::int64_t heartbeat_budget_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            config_.heartbeat_interval * config_.max_missed)
            .count();
    const std::int64_t grace_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(config_.suspend_grace).count();

    // 待回收列表：遍历中不能结构性修改 Store，统一收集后处理。
    // 复用成员缓冲，避免每 Tick 分配（§10 热路径禁止大规模分配）。
    scratch_.clear();

    store_.ForEachMutable([&](Session& s) {
        const std::int64_t elapsed_ns = ToNanos(s.last_heartbeat, now);
        if (s.state == SessionState::Active && elapsed_ns > heartbeat_budget_ns) {
            --active_count_;
            ++suspended_count_;
            ++timed_out_;
            s.state          = SessionState::Suspended;
            s.last_heartbeat = now;  // grace 计时的起点
            if (bus_ != nullptr) {
                (void)bus_->Publish(SessionSuspended{s.session_id, s.player_id,
                                                     net::CloseReason::Timeout, s.version});
            }
        } else if (s.state == SessionState::Suspended && elapsed_ns > grace_ns) {
            // §21 禁止无限保留 Suspended 会话：grace 超时立即释放
            scratch_.push_back(s.session_id);
        }
        return true;
    });

    for (const SessionId id : scratch_) {
        (void)CloseAndRemove(id, net::CloseReason::Timeout);
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::gateway
