// server/daemon/gateway_main.cpp — Gateway 进程入口（TASK-009/010/037 的进程组合层）
//
// 职责（§3）：网络连接 / Session / 心跳 / 路由。本文件把：
//   - TASK-008 INetworkTransport（TCP listen + Poll 事件流；Recv 侧已剥 4B 长度前缀，
//     Received 事件 data 即一条完整应用帧 —— 上层**不得**再做帧组装）
//   - TASK-009 SessionManager（六状态机 + 心跳 + 断线挂起）
//   - TASK-010 GatewayRouter / NodeRegistry（路由查询，第一版在进程中可扩展）
// 组合到一个可运行进程；鉴权第一版为本地校验替身（IAuthProvider stub，
// session.h §15.7 明确「真实实现由后续任务 / 部署注入」）。
//
// 应用层帧约定（第一版最小协议，正式编解码归 protocol 任务）：
//   鉴权帧 24B = [player_id u64][nonce u64][signature u64]（小端内存序 memcpy），
//     signature = player_id * 0x9E3779B97F4A7C15 ^ (nonce + 1)（与 StubAuthProvider 一致）；
//   其它任意非空帧 = 心跳（刷新 last_heartbeat）。
//
// 双形态：
//   - 独立进程：本文件 main()（四进程部署形态）
//   - All-in-One：RunGateway(args) 被 allinone_main.cpp 在独立线程调用
//     （单进程开发/小规模形态）。**两个形态不同时运行**，避免端口冲突。
//
// 主循环（调用线程独占驱动，§9 / §21）：
//   Poll(50ms) → 逐事件（Connected→OnConnected / Disconnected→OnDisconnected /
//   Received→鉴权或心跳）→ SessionManager.Tick(now) → EventBus.Drain(2ms) → 5s 摘要。

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_manager.h"
#include "mmo/gateway/session/session_store.h"
#include "mmo/net/transport.h"

#include "daemon_common.h"

namespace mmo::daemon {

using mmo::core::DurationMs;
using mmo::core::ErrorCode;
using mmo::core::MonotonicClock;
using mmo::net::CloseReason;
using mmo::net::ConnectionId;
using mmo::net::IConnection;
using mmo::net::TransportEvent;
namespace gw = mmo::gateway;

/// 鉴权替身：signature = (player_id * 黄金比率) ^ (nonce + 1)。
/// 真实口令/token 校验对接 TASK-028 repositories，由部署注入替换本类。
class StubAuthProvider final : public gw::IAuthProvider {
public:
    mmo::core::Result<gw::PlayerId> Authenticate(const gw::AuthToken& token) noexcept override {
        const std::uint64_t expect =
            token.player_id * 0x9E3779B97F4A7C15ull ^ (token.nonce + 1);
        if (token.player_id == gw::kInvalidPlayerId || token.signature != expect) {
            return mmo::core::Result<gw::PlayerId>::Fail(mmo::core::Error(
                ErrorCode::UNAUTHORIZED, "bad token signature", mmo::core::domain::kCore));
        }
        return mmo::core::Result<gw::PlayerId>::Ok(token.player_id);
    }
};

/// 24B 鉴权帧 → AuthToken（布局见文件头注释；与 smoke_client / daemon_selftest 一致）。
bool ParseAuthFrame(const std::uint8_t* data, std::size_t len, gw::AuthToken* token) {
    if (len != 24) {
        return false;
    }
    std::memcpy(&token->player_id, data, 8);
    std::memcpy(&token->nonce, data + 8, 8);
    std::memcpy(&token->signature, data + 16, 8);
    return true;
}

/// conn → session 反查（断线/鉴权/心跳均为低频路径，快照遍历足够）。
std::optional<gw::SessionId> FindSessionByConn(gw::InMemorySessionStore& store,
                                               ConnectionId conn_id) {
    for (const gw::Session& s : store.Snapshot()) {
        if (s.conn_id == conn_id && !gw::IsTerminal(s.state)) {
            return s.session_id;
        }
    }
    return std::nullopt;
}

/// Gateway 主体（进程 main 与 All-in-One 共用）。
/// 返回 0 = 优雅退出；非 0 = 启动失败（如端口占用）。
int RunGateway(const Args& args) {
    using namespace mmo::daemon;
    namespace core = mmo::core;  // daemon_common.h 内引用 core:: 类型，此处同样需要该别名

    // 配置可能已由进程壳加载过（幂等：重复 LoadDir 仅合并同键）。
    LoadConfigOrWarn(args.config_dir);

    // ---- 监听参数：CLI 覆盖 > 配置 > 默认 ----
    const std::string host =
        !args.host.empty() ? args.host : CfgOr<std::string>("network.gateway_host", "127.0.0.1");
    const auto port = args.port != 0
                          ? args.port
                          : static_cast<std::uint16_t>(CfgOr<std::uint32_t>("network.gateway_port", 9000));

    // ---- 组件组装（依赖全注入，§27.4；组合顺序 = 析构逆序安全）----
    core::EventBus bus;
    gw::InMemorySessionStore store;
    StubAuthProvider auth;
    gw::SessionManager sessions(store, auth, &bus);

    mmo::net::TcpConfig tcfg;
    tcfg.backlog = static_cast<std::int32_t>(CfgOr<std::uint32_t>("network.listen_backlog", 512));
    auto transport = mmo::net::CreateTcpTransport(tcfg);

    const auto listened = transport->Listen(host, port);
    if (!listened.HasValue()) {
        MMO_LOG_FATAL("gateway: listen {}:{} failed ({})", host, port, listened.Err().ToString());
        return 1;
    }
    MMO_LOG_INFO("gateway: listening on {}:{} (config v{})", host, port,
                 core::ConfigManager::Version());

    std::vector<TransportEvent> events;
    events.reserve(64);

    const core::SteadyTime deadline = RunDeadline(args.run_for_sec);
    core::SteadyTime last_summary = MonotonicClock::Point();
    std::uint64_t frames_in = 0;
    std::uint64_t auth_ok = 0;

    while (!g_stop.load(std::memory_order_relaxed) && !DeadlineReached(deadline)) {
        PollStopFile(args.stop_file);
        // 1. 网络 IO（50ms 预算；ev.data 已是剥好前缀的完整应用帧）
        events.clear();
        const auto polled = transport->Poll(DurationMs(50), events);
        if (!polled.HasValue()) {
            MMO_LOG_WARN("gateway: poll error {}", polled.Err().ToString());
        }
        for (const TransportEvent& ev : events) {
            switch (ev.kind) {
                case TransportEvent::Kind::Connected: {
                    const auto sid = sessions.OnConnected(ev.conn_id);
                    if (!sid.HasValue()) {
                        MMO_LOG_WARN("gateway: reject conn={} ({})", ev.conn_id,
                                     sid.Err().ToString());
                        if (IConnection* c = transport->Get(ev.conn_id)) {
                            (void)c->Close(CloseReason::Rejected);
                        }
                    } else {
                        MMO_LOG_DEBUG("gateway: conn={} -> session={}", ev.conn_id, sid.Value());
                    }
                    break;
                }
                case TransportEvent::Kind::Disconnected: {
                    const auto sid = FindSessionByConn(store, ev.conn_id);
                    if (sid.has_value()) {
                        (void)sessions.OnDisconnected(*sid, CloseReason::PeerClosed);
                    }
                    break;
                }
                case TransportEvent::Kind::Received: {
                    ++frames_in;
                    const auto sid = FindSessionByConn(store, ev.conn_id);
                    if (!sid.has_value()) {
                        break;
                    }
                    gw::AuthToken token;
                    if (ParseAuthFrame(ev.data.data(), ev.data.size(), &token)) {
                        const auto r = sessions.OnAuthenticate(*sid, token.player_id, token);
                        if (r.HasValue()) {
                            ++auth_ok;
                            MMO_LOG_DEBUG("gateway: session={} auth player={}", *sid,
                                          token.player_id);
                        } else {
                            MMO_LOG_WARN("gateway: auth failed session={} ({})", *sid,
                                         r.Err().ToString());
                            if (IConnection* c = transport->Get(ev.conn_id)) {
                                (void)c->Close(CloseReason::Graceful);
                            }
                        }
                    } else {
                        (void)sessions.OnHeartbeat(*sid);
                    }
                    break;
                }
                case TransportEvent::Kind::SendDrained:
                case TransportEvent::Kind::Error:
                    break;
            }
        }

        // 2. 会话超时扫描（心跳超时→挂起；grace 超时→回收）
        const auto now = MonotonicClock::Point();
        (void)sessions.Tick(now);

        // 3. 事件派发（2ms 预算，§15.7）
        (void)bus.Drain(1024, DurationMs(2));

        // 4. 5 秒摘要
        if (now - last_summary >= DurationMs(5000)) {
            const auto st = transport->Stats();
            MMO_LOG_INFO("gateway: conns={} active={} suspended={} frames_in={} auth_ok={}",
                         st.conn_count, sessions.ActiveCount(), sessions.SuspendedCount(),
                         frames_in, auth_ok);
            last_summary = now;
        }
    }

    // ---- 退出：停监听排空连接（日志 flush 由进程壳统一做）----
    MMO_LOG_INFO("gateway: shutting down (conns={})", transport->ConnectionCount());
    (void)transport->Stop();
    return 0;
}

}  // namespace mmo::daemon

// 独立进程入口（All-in-One 链接本文件时用 MMO_DAEMON_AS_LIBRARY 排除）
#ifndef MMO_DAEMON_AS_LIBRARY
int main(int argc, char** argv) {
    using namespace mmo::daemon;
    namespace core = mmo::core;

    Args args;
    if (!ParseArgs(argc, argv, &args) || args.help) {
        PrintUsage(argc > 0 ? argv[0] : "gateway");
        return args.help ? 0 : 1;
    }

    InitLoggerOrWarn("gateway", args.log_file, args.console);
    InstallSignalHandlers();

    LoadConfigOrWarn(args.config_dir);
    ApplyLogLevelFromConfig();

    const int rc = RunGateway(args);

    core::Logger::Flush();
    core::Logger::Shutdown();
    return rc;
}
#endif  // MMO_DAEMON_AS_LIBRARY
