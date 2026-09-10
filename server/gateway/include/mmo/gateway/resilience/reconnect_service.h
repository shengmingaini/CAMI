// server/gateway/include/mmo/gateway/resilience/reconnect_service.h — TASK-037 §7 / §15.3-15.5
//
// 玩家重连六步状态机（§8）：Disconnect → Reconnect → Authenticate → Load Player →
// Attach Scene → Resume。每步可独立失败；version 校验复用 TASK-009 SessionManager::Reattach
// （防旧连接回放）；grace 期内 Session 已由 SessionManager 保留（Suspended）。
//
// 设计（§27.3 模块边界）：本服务只消费 TASK-009 的**公开接口**（SessionManager 引用），
// 玩家数据加载 / 场景挂载通过本任务定义的**窄接口**（IPlayerDataLoader / ISceneAttacher）
// 注入，禁止 include DataService / Scene 的 src/。测试用 stub 实现这些接口。
//
// 线程模型（§9）：由 Gateway 主线程驱动，无锁（单写者）。

#pragma once

#include <cstdint>
#include <unordered_map>

#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_manager.h"
#include "mmo/net/transport.h"

namespace mmo::gateway {

/// 会话只读快照：Begin 时取出，供后续步骤使用（避免重复查 SessionManager 内部）。
struct SessionInfo {
    PlayerId     player_id{kInvalidPlayerId};
    std::uint32_t version{0};
    SceneId      scene_id{kInvalidSceneId};
    NodeId       game_node_id{0};
};

/// 会话信息查询（只读）：重连服务在 Begin / Reconnecting 步取出 player/version/scene。
class ISessionInfoProvider {
public:
    virtual ~ISessionInfoProvider() = default;
    virtual core::Result<SessionInfo> Lookup(SessionId id) const noexcept = 0;
};

/// 玩家数据加载（§8 Load Player）：从 DataService 载入最近可靠状态。
class IPlayerDataLoader {
public:
    virtual ~IPlayerDataLoader() = default;
    virtual core::Result<void> Load(PlayerId player) noexcept = 0;
};

/// 场景挂载（§8 Attach Scene）：把会话绑定到新 GameNode。
class ISceneAttacher {
public:
    virtual ~ISceneAttacher() = default;
    virtual core::Result<NodeId> Attach(PlayerId player, SceneId scene) noexcept = 0;
};

/// 重连六步（§8，顺序固定）。
enum class ReconnectStep : std::uint8_t {
    Disconnected    = 0,  // 连接已断，Session 在 grace 期保留
    Reconnecting    = 1,  // 调用 SessionManager::Reattach（version 校验）
    Authenticating  = 2,  // 重新校验玩家身份
    LoadingPlayer   = 3,  // 从 DataService 载入玩家状态
    AttachingScene  = 4,  // 绑定新 GameNode / Scene
    Resumed         = 5,  // 重连完成
    Failed          = 6,  // 任一步失败
};

const char* ToString(ReconnectStep step) noexcept;

struct ReconnectState {
    ReconnectStep      step{ReconnectStep::Disconnected};
    net::ConnectionId  conn{0};
    SessionInfo        info{};
    core::TraceID      trace{0};
    std::uint32_t      attempts{0};
};

/// 重连编排服务（§7 / §15.3）。
class ReconnectService {
public:
    ReconnectService(SessionManager& smgr, ISessionInfoProvider& info,
                     IPlayerDataLoader& loader, ISceneAttacher& attacher,
                     core::EventBus* bus = nullptr);

    /// 启动重连：记录待接管连接，进入 Disconnected。
    core::Result<ReconnectStep> Begin(SessionId id, net::ConnectionId conn,
                                      core::TraceID trace);

    /// 推进状态机一步；返回新步骤（Failed / Resumed 为终态，不再推进）。
    core::Result<ReconnectStep> Advance(SessionId id, core::TraceID trace);

    /// 收尾：清理重连上下文。
    core::Result<void> OnComplete(SessionId id);

    ReconnectStep StatusOf(SessionId id) const noexcept;

    std::size_t ActiveCount() const noexcept { return states_.size(); }
    std::size_t ResumedCount() const noexcept { return resumed_; }
    std::size_t FailedCount() const noexcept { return failed_; }

private:
    SessionManager&        smgr_;
    ISessionInfoProvider&  info_;
    IPlayerDataLoader&     loader_;
    ISceneAttacher&        attacher_;
    core::EventBus*        bus_;
    std::unordered_map<SessionId, ReconnectState> states_;
    std::size_t           failed_{0};
    std::size_t           resumed_{0};
};

}  // namespace mmo::gateway
