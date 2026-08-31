// server/gateway/include/mmo/gateway/session/session.h — TASK-009 Session 管理
//
// §7 Public Interface：Session 结构、六状态机、ID 类型。
// §8 Data Model：Connecting → Authenticating → Active ⇄ Suspended → Closing → Closed。
//
// 依赖方向（§27.3）：server/gateway → engine/core（error/time/trace）+ engine/net（ConnectionId）。
// 红线：公开头禁止 include 内部 src/；Session 内禁止保存玩家最终持久化数据（§21，
//       那是 DataService 职责）；单会话内存 < 256B（§22）。
//
// ID 类型说明：SessionId / PlayerId / GatewayId / NodeId / SceneId 在全项目首次出现，
// 按 §27.3「只落在自身 module 子树」原则归本模块所有，其它模块请 include 本头复用。

#pragma once

#include <cstdint>

#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"
#include "mmo/net/transport.h"

namespace mmo::gateway {

// ---------------------------------------------------------------------------
// ID 类型（§7 Public Interface）
// ---------------------------------------------------------------------------

using SessionId = std::uint64_t;
using PlayerId  = std::uint64_t;
using GatewayId = std::uint32_t;
using NodeId    = std::uint32_t;
using SceneId   = std::uint32_t;

/// 0 保留为非法值（与 EventBus SubId、net ConnectionId 的约定一致）。
inline constexpr SessionId kInvalidSessionId = 0;
inline constexpr PlayerId  kInvalidPlayerId  = 0;
inline constexpr SceneId   kInvalidSceneId   = 0;

// ---------------------------------------------------------------------------
// SessionId 编解码：slot(32) + generation(32)
//
// generation 用于防 ABA：槽位复用后旧 SessionId 立即失效，杜绝「旧连接回放」
// 与「断线重连后旧引用误命中新会话」。与 net::ConnectionId 同思路。
// ---------------------------------------------------------------------------

constexpr SessionId MakeSessionId(std::uint32_t slot, std::uint32_t generation) noexcept {
    return (static_cast<SessionId>(generation) << 32) | static_cast<SessionId>(slot);
}
constexpr std::uint32_t SessionSlot(SessionId id) noexcept {
    return static_cast<std::uint32_t>(id & 0xFFFFFFFFull);
}
constexpr std::uint32_t SessionGeneration(SessionId id) noexcept {
    return static_cast<std::uint32_t>(id >> 32);
}

// ---------------------------------------------------------------------------
// 六状态机（§8 Data Model）
// ---------------------------------------------------------------------------

enum class SessionState : std::uint8_t {
    Connecting     = 0,  // 连接已建立，等待鉴权请求
    Authenticating = 1,  // 鉴权进行中（IAuthProvider 校验）
    Active         = 2,  // 正常在线，心跳活跃
    Suspended      = 3,  // 断线，grace 期内保留（可 Reattach）
    Closing        = 4,  // 主动关闭中（发送缓冲排空 / 资源回收）
    Closed         = 5,  // 终态，槽位可回收
};

/// 状态名（日志 / 测试断言用；未知值返回 "UNKNOWN"，禁止崩溃）。
const char* ToString(SessionState state) noexcept;

/// 是否终态（Closed 为唯一终态，Closing 仍可能回落到 Closed）。
constexpr bool IsTerminal(SessionState state) noexcept {
    return state == SessionState::Closed;
}

// ---------------------------------------------------------------------------
// Session（§7 七项规范字段 + 内部字段）
//
// 七项规范字段：session_id / player_id / gateway_id / game_node_id / scene_id /
//               version / last_heartbeat。
// 内部字段：state / conn_id / trace_id（不含任何玩家持久化数据，§21）。
//
// 布局刻意按 8→4→1 降序排列以最小化填充：实测 sizeof(Session) = 64 字节。
// ---------------------------------------------------------------------------

struct Session {
    SessionId           session_id{kInvalidSessionId};
    PlayerId            player_id{kInvalidPlayerId};
    core::SteadyTime    last_heartbeat{};      // 最近一次心跳（单调时钟）
    net::ConnectionId   conn_id{0};            // 当前绑定的传输层连接（Suspended 时为旧值）
    core::TraceID       trace_id{0};           // 串联日志（TASK-002）
    GatewayId           gateway_id{0};
    NodeId              game_node_id{0};
    SceneId             scene_id{kInvalidSceneId};
    std::uint32_t       version{0};            // 每次 Reattach +1，防旧连接回放
    SessionState        state{SessionState::Connecting};
};

// §22 硬约束：单会话内存占用 < 256B（编译期断言，结构体改动立即失败）。
static_assert(sizeof(Session) <= 256, "TASK-009 §22: 单会话内存占用必须 < 256B");

// ---------------------------------------------------------------------------
// 会话事件（§15.8：五种事件全部走 EventBus，由宿主 Drain 驱动派发）
//
// 事件为纯数据结构（POD），不含业务回调；订阅方禁止在回调里做阻塞 IO（§10 热路径）。
// ---------------------------------------------------------------------------

struct SessionCreated {
    SessionId         session_id{kInvalidSessionId};
    net::ConnectionId conn_id{0};
    core::TraceID     trace_id{0};
};

struct SessionAuthenticated {
    SessionId     session_id{kInvalidSessionId};
    PlayerId      player_id{kInvalidPlayerId};
    std::uint32_t version{0};
    core::TraceID trace_id{0};
};

struct SessionSuspended {
    SessionId          session_id{kInvalidSessionId};
    PlayerId           player_id{kInvalidPlayerId};
    net::CloseReason   reason{net::CloseReason::Graceful};
    std::uint32_t      version{0};
};

struct SessionResumed {
    SessionId     session_id{kInvalidSessionId};
    PlayerId      player_id{kInvalidPlayerId};
    std::uint32_t new_version{0};
    std::uint32_t old_version{0};
};

struct SessionClosed {
    SessionId          session_id{kInvalidSessionId};
    PlayerId           player_id{kInvalidPlayerId};
    net::CloseReason   reason{net::CloseReason::Graceful};
    std::uint32_t      version{0};
};

// ---------------------------------------------------------------------------
// 鉴权抽象（§15.7 / §21：禁止把鉴权口令写死在代码里）
//
// 第一版提供本地校验替身（测试用），真实实现由后续任务 / 部署注入。
// ---------------------------------------------------------------------------

struct AuthToken {
    PlayerId           player_id{kInvalidPlayerId};
    std::uint64_t      nonce{0};       // 防重放
    std::uint64_t      signature{0};   // 签名摘要（第一版为校验和替身，非真实密码学）
};

/// 鉴权返回：成功携带 PlayerId；失败返回 Error（UNAUTHORIZED）。
class IAuthProvider {
public:
    virtual ~IAuthProvider() = default;
    /// 热路径禁止阻塞 IO：真实实现须本地校验或查缓存，不得同步访问数据库 / 远程服务。
    virtual core::Result<PlayerId> Authenticate(const AuthToken& token) noexcept = 0;
};

}  // namespace mmo::gateway
