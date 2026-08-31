// server/gateway/tests/session_bench.cpp — TASK-009 §18 Benchmark
//
// 指标（机器可读 key=value，供验收脚本 assert_metric 解析）：
//   session_heartbeat_ns=   单次心跳处理耗时（ns），§22 < 200ns
//   session_tick_us_10k=    10K 会话 Tick 全量扫描耗时（us），§22 < 1ms = 1000us
//   reattach_ns=            单次断线 + 重连接管耗时（ns），§22 < 10us = 10000ns
//   per_session_bytes=      单会话内存占用（字节），§22 < 256
//
// 用法：session_bench [--sessions N]   默认 10000（与验收脚本一致）
// 输出：bench/gateway_session.txt

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_manager.h"
#include "mmo/gateway/session/session_store.h"

namespace {

using namespace mmo::gateway;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

namespace core = mmo::core;
namespace net  = mmo::net;

/// 鉴权替身（与测试共用同一替身逻辑）：signature = player_id ^ nonce 视为合法。
class BenchAuth final : public IAuthProvider {
public:
    core::Result<PlayerId> Authenticate(const AuthToken& token) noexcept override {
        if (token.player_id == kInvalidPlayerId) {
            return core::Result<PlayerId>::Fail(core::Error{
                core::ErrorCode::UNAUTHORIZED, "empty player", core::domain::kNet});
        }
        return core::Result<PlayerId>::Ok(token.player_id);
    }
};

/// MonotonicClock::Now() 直接返回纳秒（SteadyNs = int64_t），不是 time_point，
/// 无需 duration_cast（duration_cast<nanoseconds>(int64) 无匹配重载）。
std::int64_t NanosNow() noexcept {
    return mmo::core::MonotonicClock::Now();
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t sessions = 10000;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--sessions") {
            sessions = static_cast<std::size_t>(std::stoull(argv[i + 1]));
        }
    }

    InMemorySessionStore store;
    BenchAuth            auth;
    SessionManager::Config cfg{};
    SessionManager mgr(store, auth, nullptr, cfg);

    // ---- 1. 建 N 个 Active 会话 ----
    std::vector<SessionId> ids;
    ids.reserve(sessions);
    for (std::size_t i = 0; i < sessions; ++i) {
        const auto id = mgr.OnConnected(static_cast<net::ConnectionId>(i + 1));
        if (!id.HasValue()) {
            ErrorFmt("session_bench: OnConnected failed at %zu\n", i);
            return 1;
        }
        const PlayerId player = static_cast<PlayerId>(i + 1);
        if (!mgr.OnAuthenticate(id.Value(), player, AuthToken{player, 0, 0}).HasValue()) {
            ErrorFmt("session_bench: OnAuthenticate failed at %zu\n", i);
            return 1;
        }
        ids.push_back(id.Value());
    }

    // ---- 2. 心跳（§22 < 200ns/次）----
    const std::int64_t hb_start = NanosNow();
    for (const SessionId id : ids) {
        (void)mgr.OnHeartbeat(id);
    }
    const double session_heartbeat_ns =
        static_cast<double>(NanosNow() - hb_start) / static_cast<double>(sessions);

    // ---- 3. Tick 全量扫描（§22 10K < 1ms）----
    // 取 20 次平均，抹掉单次抖动；会话全部 Active 且刚刷过心跳，不会触发状态迁移，
    // 因此测的是纯粹的扫描开销。
    constexpr int kTickRounds = 20;
    const auto now = mmo::core::MonotonicClock::Point();
    const std::int64_t tick_start = NanosNow();
    for (int r = 0; r < kTickRounds; ++r) {
        (void)mgr.Tick(now);
    }
    const double session_tick_us_10k =
        static_cast<double>(NanosNow() - tick_start) / static_cast<double>(kTickRounds) / 1000.0;

    // ---- 4. 断线 + 重连（§22 Reattach < 10us）----
    const std::int64_t re_start = NanosNow();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        (void)mgr.OnDisconnected(ids[i], net::CloseReason::PeerClosed);
        (void)mgr.Reattach(ids[i], static_cast<net::ConnectionId>(i + 1),
                           /*expected_version=*/0);
    }
    const double reattach_ns =
        static_cast<double>(NanosNow() - re_start) / static_cast<double>(sessions);

    // ---- 5. 单会话内存占用（§22 < 256B）----
    const double per_session_bytes =
        static_cast<double>(store.AllocatedBytes()) / static_cast<double>(sessions);

    // ---- 输出 ----
    Line("== TASK-009 session_bench ==");
    ErrorFmt("sessions=%zu active=%zu\n", sessions, mgr.ActiveCount());

    std::ofstream ofs("bench/gateway_session.txt");
    if (!ofs) {
        ErrorFmt("session_bench: cannot write bench/gateway_session.txt (cwd must be repo root)\n");
        return 1;
    }
    ofs << "sessions=" << sessions << "\n";
    ofs << "session_heartbeat_ns=" << session_heartbeat_ns << "\n";
    ofs << "session_tick_us_10k=" << session_tick_us_10k << "\n";
    ofs << "reattach_ns=" << reattach_ns << "\n";
    ofs << "per_session_bytes=" << per_session_bytes << "\n";
    ofs.close();

    ErrorFmt("session_heartbeat_ns=%.2f\n", session_heartbeat_ns);
    ErrorFmt("session_tick_us_10k=%.2f\n", session_tick_us_10k);
    ErrorFmt("reattach_ns=%.2f\n", reattach_ns);
    ErrorFmt("per_session_bytes=%.2f\n", per_session_bytes);
    ErrorFmt("session_bench done -> bench/gateway_session.txt\n");
    return 0;
}
