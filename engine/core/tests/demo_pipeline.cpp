// TASK-007 §15.8 · Demo 全链路：Command → Handler → Event → 2 × Subscriber
//
// 这条链路用来证明三态总线的**语义边界**是真的成立，而不只是能编译：
//   1. Command 执行完立即拿到返回值（同步）；
//   2. 但 Event 只是**入队**，此刻订阅者还没收到（异步，符合 §8「Event 默认异步」）；
//   3. 宿主线程在 Tick 的 Event 阶段 Drain 之后，订阅者才按注册顺序收到；
//   4. Query 全程零副作用（SideEffectProbe 违规计数恒为 0）。
//
// 业务状态（玩家坐标）由场景侧自己持有 —— 总线只搬运，不拥有任何业务状态（§4）。

#include "test_print.h"

#include <cstdint>
#include <string>
#include <vector>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/bus/query_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"

#include "bus_fixtures.h"

namespace {

using mmo::core::CommandBus;
using mmo::core::CommandContext;
using mmo::core::DeriveRequestID;
using mmo::core::DurationMs;
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::EventBus;
using mmo::core::NewTraceID;
using mmo::core::QueryBus;
using mmo::core::QueryContext;
using mmo::core::Result;
using mmo::core::SideEffectProbe;
using mmo::core::TraceID;
// domain 是命名空间（error_code.h 里的受控错误域集合），只能用别名引入。
namespace domain = mmo::core::domain;

namespace fx = mmo::core::bus_fixtures;

int g_failures = 0;

#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__, \
                                        #cond);                                    \
            ++g_failures;                                                          \
        }                                                                          \
    } while (false)

template <typename T>
T CheckOk(const Result<T>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return T{};
    }
    return result.Value();
}

inline bool CheckOk(const Result<void>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return false;
    }
    return true;
}

#define EXPECT_OK(expr) CheckOk((expr), #expr, __FILE__, __LINE__)

namespace tprint = ::mmo::core::test;

/// 场景侧业务状态（总线不拥有它，只通过 Command 修改）。
struct SceneState {
    float x{0.0f};
    float y{0.0f};
};

}  // namespace

int main() {
    tprint::Line("=== TASK-007 Demo: Command -> Handler -> Event -> Subscribers ===\n");

    CommandBus commands;
    QueryBus queries;
    EventBus events;
    SceneState state;
    SideEffectProbe probe;

    // ---- 1) Command Handler：改状态 + 发布「已发生的事实」 ----------------
    const bool cmd_ok = EXPECT_OK(commands.RegisterFn<fx::MovePlayerCommand>(
        [&state, &events](const fx::MovePlayerCommand& cmd,
                          const CommandContext& ctx) -> Result<fx::Position> {
            state.x += cmd.dx;
            state.y += cmd.dy;

            fx::PlayerMovedEvent ev;
            ev.player_id = cmd.player_id;
            ev.scene_id = ctx.scene_id;
            ev.x = static_cast<std::int32_t>(state.x);
            ev.y = static_cast<std::int32_t>(state.y);
            ev.request_id = ctx.request_id;
            (void)events.Publish(ev);  // 入队，**不立即派发**
            return Result<fx::Position>::Ok(fx::Position{state.x, state.y});
        }));
    CHECK(cmd_ok);

    // ---- 2) Query Handler：只读，禁止副作用 -------------------------------
    const bool query_ok = EXPECT_OK(queries.RegisterFn<fx::GetPositionQuery>(
        [&state, &probe](const fx::GetPositionQuery& q,
                         const QueryContext&) -> Result<fx::Position> {
            // 只读实现：全程不写任何状态（probe 保持零写入、零违规）。
            // 「写入会被捕获」的反例由 bus_test.cpp 的 TestQueryNoSideEffect 覆盖。
            (void)probe;
            if (q.player_id == 0) {
                return Result<fx::Position>::Fail(
                    Error(ErrorCode::INVALID_ARGUMENT, "bad player", domain::kCore));
            }
            return Result<fx::Position>::Ok(fx::Position{state.x, state.y});
        }));
    CHECK(query_ok);

    // ---- 3) 两个订阅者：AOI 广播 + 统计（按注册顺序派发） ----------------
    std::vector<std::string> log;
    (void)EXPECT_OK(events.Subscribe<fx::PlayerMovedEvent>(
        [&log](const fx::PlayerMovedEvent& e) { log.push_back("aoi:" + std::to_string(e.player_id)); }));
    (void)EXPECT_OK(events.Subscribe<fx::PlayerMovedEvent>(
        [&log](const fx::PlayerMovedEvent& e) { log.push_back("stat:" + std::to_string(e.x)); }));
    CHECK(events.SubscriberCount() == 2);

    // ---- 4) 派发命令（客户端来源，带 trace/request id） -------------------
    const TraceID trace = NewTraceID();
    CommandContext ctx;
    ctx.trace_id = trace;
    ctx.request_id = DeriveRequestID(trace);
    ctx.player_id = 1001;
    ctx.scene_id = 7;
    ctx.source = mmo::core::CommandSource::kClient;

    fx::MovePlayerCommand cmd;
    cmd.request_id = ctx.request_id;
    cmd.player_id = 1001;
    cmd.source = mmo::core::CommandSource::kClient;
    cmd.timestamp = 1700000000;
    cmd.version = 1;
    cmd.dx = 5.0f;
    cmd.dy = -2.0f;

    const fx::Position after = EXPECT_OK(commands.Dispatch(cmd, ctx));
    CHECK(after.x == 5.0f);
    CHECK(after.y == -2.0f);

    // ---- 5) 异步语义：命令已生效，但事件尚未派发 --------------------------
    CHECK(events.QueueDepth() == 1);
    CHECK(log.empty());

    // ---- 6) Query 只读查询（Verify 零副作用） -----------------------------
    fx::GetPositionQuery query;
    query.player_id = 1001;
    QueryContext qctx;
    qctx.trace_id = trace;
    const fx::Position queried = EXPECT_OK(queries.Ask(query, qctx));
    CHECK(queried.x == 5.0f);
    CHECK(queried.y == -2.0f);
    CHECK(probe.Violations() == 0);  // Query 期间没有发生任何写操作

    // ---- 7) 宿主 Tick 的 Event 阶段：Drain 派发 ---------------------------
    const std::size_t remaining = EXPECT_OK(events.Drain(64, DurationMs(2)));
    CHECK(remaining == 0);
    CHECK(log.size() == 2);
    if (log.size() == 2) {
        CHECK(log[0] == "aoi:1001");  // 注册顺序 + payload 一致
        CHECK(log[1] == "stat:5");
    }

    // ---- 8) 第二条命令：状态累加，事件继续流转 ----------------------------
    fx::MovePlayerCommand cmd2;
    cmd2.player_id = 1001;
    cmd2.dx = 2.0f;
    cmd2.dy = 3.0f;
    const fx::Position after2 = EXPECT_OK(commands.Dispatch(cmd2, ctx));
    CHECK(after2.x == 7.0f);
    CHECK(after2.y == 1.0f);

    CHECK(EXPECT_OK(events.Drain(64, DurationMs(2))) == 0);
    CHECK(log.size() == 4);
    if (log.size() == 4) {
        CHECK(log[2] == "aoi:1001");
        CHECK(log[3] == "stat:7");  // 累加后的 x
    }
    CHECK(events.QueueDepth() == 0);
    CHECK(events.DroppedCount() == 0);
    CHECK(events.SubscriberErrors() == 0);
    CHECK(probe.Violations() == 0);

    // ---- 9) 关键事件（经济类）全链路零丢弃 -------------------------------
    (void)EXPECT_OK(events.Subscribe<fx::EconomyEvent>(
        [&log](const fx::EconomyEvent&) { log.push_back("economy"); }));
    for (int i = 0; i < 100; ++i) {
        CHECK(events.Publish(fx::EconomyEvent{1001, 10, 9001}).HasValue());
    }
    CHECK(EXPECT_OK(events.Drain(1000, DurationMs(5))) == 0);
    CHECK(events.DroppedCount() == 0);
    CHECK(log.size() == 104);

    if (g_failures == 0) {
        tprint::Line("DEMO PIPELINE PASSED\n");
        return 0;
    }
    tprint::ErrorFmt("%d DEMO CHECK(S) FAILED\n", g_failures);
    return 1;
}
