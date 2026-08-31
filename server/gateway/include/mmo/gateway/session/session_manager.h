// server/gateway/include/mmo/gateway/session/session_manager.h — TASK-009 §15.3~8
//
// SessionManager：六状态机 + 心跳检测 + 断线挂起 + 重连接管（Reattach）。
//
// 线程模型（§9）：由 Gateway 的 NetworkThread 独占驱动，**无任何锁**
// （§21 禁止用全局锁保护 Session 表）；跨线程查询走 Snapshot 或事件。
// 热路径（§10）：心跳 / Tick 内禁止阻塞 IO、禁止大规模分配。
//
// 时间源：一律 core::MonotonicClock（禁止 WallClock 做超时判定，PROJECT_REQUIREMENTS §13）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/session/session.h"
#include "mmo/gateway/session/session_store.h"

namespace mmo::gateway {

class SessionManager final {
public:
    /// §7 Config（签名与默认值严格按任务书）。
    struct Config {
        core::DurationMs heartbeat_interval{5000};  // 期望心跳间隔
        std::uint32_t    max_missed{3};             // 允许丢失的心跳次数
        core::DurationMs suspend_grace{30000};      // Suspended 保留时长
        std::size_t      max_sessions{50000};       // 容量上限（禁止无界增长）
    };

    /// store / auth 必填；bus 可空（空则不发事件，便于纯逻辑单测）。
    /// 依赖全注入、不使用全局单例（§27.4 扩展性：新增实现不得修改既有任务文件）。
    SessionManager(InMemorySessionStore& store, IAuthProvider& auth,
                   core::EventBus* bus = nullptr);

    /// 带自定义配置。Config 是嵌套类型且带 NSDMI，GCC 下无法写作默认实参
    /// （`Config config = {}` 编译失败，TASK-007 同坑），故用重载而非默认值。
    SessionManager(InMemorySessionStore& store, IAuthProvider& auth,
                   core::EventBus* bus, Config config);

    // ---- §7 Public Interface（签名冻结，禁止破坏性变更）----

    /// 新连接接入：创建 Connecting 会话。达容量上限返回 BUSY（§15.6）。
    core::Result<SessionId> OnConnected(net::ConnectionId conn_id);

    /// 鉴权：Connecting/Authenticating → Active。
    /// 鉴权失败转 Closed 并回收（§19 不保留悬挂会话），返回 UNAUTHORIZED。
    core::Result<void> OnAuthenticate(SessionId id, PlayerId player, const AuthToken& token);

    /// 心跳：仅 Active 会话接受，刷新 last_heartbeat（§22 < 200ns/次）。
    core::Result<void> OnHeartbeat(SessionId id);

    /// 断线：Active → Suspended（记录断线时刻，开始 grace 计时）；
    /// 未完成鉴权的会话直接 Closed 并回收。
    core::Result<void> OnDisconnected(SessionId id, net::CloseReason reason);

    /// 重连接管：Suspended（grace 内）→ Active，version+1。
    /// expected_version 不匹配返回 VERSION_CONFLICT（§15.5 防旧连接回放）。
    /// 旧连接仍在线（Active）时强制接管，先发 SessionClosed 再发 SessionResumed（§19）。
    core::Result<void> Reattach(SessionId id, net::ConnectionId new_conn,
                                std::uint32_t expected_version);

    /// 宿主驱动的超时扫描：心跳超时 → Suspended；grace 超时 → Closed 并回收。
    core::Result<void> Tick(core::SteadyTime now);

    std::size_t ActiveCount() const noexcept { return active_count_; }
    std::size_t SuspendedCount() const noexcept { return suspended_count_; }

    // ---- 观测指标（供测试 / bench 断言，非契约）----
    std::size_t TotalCount() const noexcept { return active_count_ + suspended_count_; }
    std::size_t RejectedReattachCount() const noexcept { return rejected_reattach_; }
    std::size_t TimedOutCount() const noexcept { return timed_out_; }

private:
    /// 读取会话（Store 接口返回拷贝，因此状态变更遵循 Load → 改 → Save 模式）。
    /// 心跳热路径开销 = 两次 64B 拷贝 + O(1) 寻址，实测 < 200ns（§22）。
    core::Result<Session> Load(SessionId id);
    core::Result<void>    Save(const Session& s);
    void                  PublishCreated(const Session& s);
    void                  PublishClosed(const Session& s, net::CloseReason reason);
    core::Result<void>    CloseAndRemove(SessionId id, net::CloseReason reason);

    InMemorySessionStore& store_;
    IAuthProvider&        auth_;
    core::EventBus*       bus_;
    Config                config_;

    // O(1) 计数（§20.5 要求 Tick < 1ms，计数避免每次全量统计）
    std::size_t active_count_{0};
    std::size_t suspended_count_{0};
    std::size_t rejected_reattach_{0};  // version 不匹配次数
    std::size_t timed_out_{0};          // 心跳超时转 Suspended 次数

    // Tick 待回收会话的复用缓冲：每 Tick clear 复用，避免热路径反复分配（§10）。
    std::vector<SessionId> scratch_;
};

}  // namespace mmo::gateway
