// server/gateway/src/resilience/reconnect_service.cpp — TASK-037 §7 / §15.3-15.5
//
// 玩家重连六步状态机编排。单写者（Gateway 主线程），无锁。
// 关键：Reconnecting 步调用 SessionManager::Reattach 时**重新取最新 version**，
// 保证重试不会因上一次 Reattach 已 bump 版本而落入 VERSION_CONFLICT（防回放 ≠ 防重试）。

#include "mmo/gateway/resilience/reconnect_service.h"

#include <algorithm>

namespace mmo::gateway {
namespace {

using core::ErrorCode;

const char* kUnknownStep = "Unknown";

}  // namespace

const char* ToString(ReconnectStep step) noexcept {
    switch (step) {
        case ReconnectStep::Disconnected:    return "Disconnected";
        case ReconnectStep::Reconnecting:    return "Reconnecting";
        case ReconnectStep::Authenticating:  return "Authenticating";
        case ReconnectStep::LoadingPlayer:   return "LoadingPlayer";
        case ReconnectStep::AttachingScene:  return "AttachingScene";
        case ReconnectStep::Resumed:         return "Resumed";
        case ReconnectStep::Failed:          return "Failed";
        default:                             return kUnknownStep;
    }
}

ReconnectService::ReconnectService(SessionManager& smgr, ISessionInfoProvider& info,
                                   IPlayerDataLoader& loader, ISceneAttacher& attacher,
                                   core::EventBus* bus)
    : smgr_(smgr), info_(info), loader_(loader), attacher_(attacher), bus_(bus) {}

core::Result<ReconnectStep> ReconnectService::Begin(SessionId id, net::ConnectionId conn,
                                                     core::TraceID trace) {
    auto looked = info_.Lookup(id);
    if (!looked.HasValue()) {
        return core::Result<ReconnectStep>::Fail(looked.Err());
    }
    ReconnectState s{};
    s.step   = ReconnectStep::Disconnected;
    s.conn   = conn;
    s.info   = looked.Value();
    s.trace  = trace;
    s.attempts = 0;
    states_[id] = s;
    return core::Result<ReconnectStep>::Ok(ReconnectStep::Disconnected);
}

core::Result<ReconnectStep> ReconnectService::Advance(SessionId id, core::TraceID trace) {
    auto it = states_.find(id);
    if (it == states_.end()) {
        return core::Result<ReconnectStep>::Fail(
            core::Error{ErrorCode::NOT_FOUND, "no reconnect state", core::domain::kNet});
    }
    ReconnectState& s = it->second;
    s.attempts += 1;
    s.trace = trace;

    switch (s.step) {
        case ReconnectStep::Disconnected: {
            // 进入重连态（Reattach 在下一步执行）。
            s.step = ReconnectStep::Reconnecting;
            break;
        }
        case ReconnectStep::Reconnecting: {
            // 取最新 version 后调用 SessionManager::Reattach（校验防旧连接回放）；
            // 重试安全：每次都用 info provider 返回的最新 version，避免上一次 bump 导致 VERSION_CONFLICT。
            auto cur = info_.Lookup(id);
            if (!cur.HasValue()) {
                s.step = ReconnectStep::Failed;
                ++failed_;
                break;
            }
            s.info.version = cur.Value().version;
            auto r = smgr_.Reattach(id, s.conn, s.info.version);
            if (!r.HasValue()) {
                s.step = ReconnectStep::Failed;
                ++failed_;
                break;
            }
            s.step = ReconnectStep::Authenticating;
            break;
        }
        case ReconnectStep::Authenticating: {
            // Load Player：从 DataService 载入最近可靠玩家状态。
            auto r = loader_.Load(s.info.player_id);
            if (!r.HasValue()) {
                s.step = ReconnectStep::Failed;
                ++failed_;
                break;
            }
            s.step = ReconnectStep::LoadingPlayer;
            break;
        }
        case ReconnectStep::LoadingPlayer: {
            // Attach Scene：绑定新 GameNode。
            auto r = attacher_.Attach(s.info.player_id, s.info.scene_id);
            if (!r.HasValue()) {
                s.step = ReconnectStep::Failed;
                ++failed_;
                break;
            }
            s.step = ReconnectStep::AttachingScene;
            break;
        }
        case ReconnectStep::AttachingScene: {
            s.step = ReconnectStep::Resumed;
            ++resumed_;
            break;
        }
        case ReconnectStep::Resumed:
        case ReconnectStep::Failed:
        default:
            break;  // 终态：不再推进
    }
    return core::Result<ReconnectStep>::Ok(s.step);
}

core::Result<void> ReconnectService::OnComplete(SessionId id) {
    states_.erase(id);
    return core::Result<void>::Ok();
}

ReconnectStep ReconnectService::StatusOf(SessionId id) const noexcept {
    auto it = states_.find(id);
    if (it == states_.end()) {
        return ReconnectStep::Disconnected;  // 未登记视为无重连
    }
    return it->second.step;
}

}  // namespace mmo::gateway
