// TASK-007 · Command / Query / Event Bus —— 单元 / 集成 / Failure 测试
//
// 自包含 harness（不依赖 gtest，与 TASK-001~004 同风格）。
// 输出一律走 test_print.h，遵守 TASK-000「engine/ 内禁止 cout / printf / cerr」红线。
//
// 覆盖矩阵：
//   §16 单元测试    : Command 注册/重复注册/未注册/异常、Query Ask/错误传播/无副作用、
//                     Event 多播顺序/退订幂等/大事件堆路径
//   §17 集成测试    : Demo 全链路（见 demo_pipeline.cpp）、跨线程 trace 透传、混合负载无死锁
//   §19 Failure 测试: Handler 抛异常、订阅者异常隔离、队列满背压（非关键丢弃 / 关键 BUSY）、
//                     Drain 预算耗尽返回剩余

#include "test_print.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
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
using mmo::core::EventBusOptions;
using mmo::core::NewTraceID;
using mmo::core::QueryBus;
using mmo::core::QueryContext;
using mmo::core::Result;
using mmo::core::SideEffectProbe;
using mmo::core::TraceID;
using mmo::core::kInvalidTraceId;

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

// 取值前必须先判 HasValue：Result::Value() 对错误结果会抛 bad_variant_access，
// 那样只会得到看不懂的崩溃，而不是「哪一行断言失败」。
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

// ---- §16 CommandBus：注册 / 派发 / 返回值 --------------------------------

void TestCommandRegisterDispatch() {
    CommandBus bus;
    fx::Position state{0.0f, 0.0f};

    const bool registered = EXPECT_OK(bus.RegisterFn<fx::MovePlayerCommand>(
        [&state](const fx::MovePlayerCommand& cmd, const CommandContext& ctx) -> Result<fx::Position> {
            if (ctx.trace_id == kInvalidTraceId) {
                return Result<fx::Position>::Fail(
                    Error(ErrorCode::INVALID_ARGUMENT, "missing trace", domain::kCore));
            }
            state.x += cmd.dx;
            state.y += cmd.dy;
            return Result<fx::Position>::Ok(state);
        }));
    CHECK(registered);
    CHECK(bus.RegisteredCount() == 1);

    const TraceID trace = NewTraceID();
    CommandContext ctx;
    ctx.trace_id = trace;
    ctx.request_id = DeriveRequestID(trace);
    ctx.player_id = 42;
    ctx.scene_id = 7;

    fx::MovePlayerCommand cmd;
    cmd.request_id = ctx.request_id;
    cmd.player_id = 42;
    cmd.timestamp = 1700000000;
    cmd.dx = 3.0f;
    cmd.dy = -1.0f;

    const fx::Position moved = EXPECT_OK(bus.Dispatch(cmd, ctx));
    CHECK(moved.x == 3.0f);
    CHECK(moved.y == -1.0f);
    CHECK(bus.InFlight() == 0);  // 同步执行，返回后在途归零
}

// ---- §19 重复注册必须报错且**不覆盖**（防静默覆盖） ----------------------

void TestCommandDuplicateRegisterNotOverwrite() {
    CommandBus bus;
    int first_calls = 0;

    CHECK(EXPECT_OK(bus.RegisterFn<fx::MovePlayerCommand>(
        [&first_calls](const fx::MovePlayerCommand&, const CommandContext&) -> Result<fx::Position> {
            ++first_calls;
            return Result<fx::Position>::Ok(fx::Position{1.0f, 1.0f});
        })));

    const auto dup = bus.RegisterFn<fx::MovePlayerCommand>(
        [](const fx::MovePlayerCommand&, const CommandContext&) -> Result<fx::Position> {
            return Result<fx::Position>::Ok(fx::Position{9.0f, 9.0f});
        });
    CHECK(!dup.HasValue());
    CHECK(dup.Err().Code() == ErrorCode::INVALID_ARGUMENT);
    CHECK(bus.RegisteredCount() == 1);

    // 原 handler 仍然生效（若被覆盖，这里会拿到 9.0f）
    const fx::Position p = EXPECT_OK(bus.Dispatch(fx::MovePlayerCommand{}, CommandContext{}));
    CHECK(p.x == 1.0f);
    CHECK(first_calls == 1);
}

void TestCommandNotFound() {
    CommandBus bus;
    const auto r = bus.Dispatch(fx::MovePlayerCommand{}, CommandContext{});
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == ErrorCode::NOT_FOUND);
}

// ---- §19 Handler 抛异常 → INTERNAL_ERROR，总线可继续服务 ------------------

void TestCommandHandlerThrows() {
    CommandBus bus;
    CHECK(EXPECT_OK(bus.RegisterFn<fx::MovePlayerCommand>(
        [](const fx::MovePlayerCommand&, const CommandContext&) -> Result<fx::Position> {
            throw std::runtime_error("boom");
            // 不可达的 return：仅为满足返回类型 —— 缺少它时 GCC -O3 会生成
            // 「落到函数末尾」的未定义行为，表现为段错误而非异常。
            return Result<fx::Position>::Ok(fx::Position{});
        })));

    const auto r = bus.Dispatch(fx::MovePlayerCommand{}, CommandContext{});
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == ErrorCode::INTERNAL_ERROR);
    CHECK(bus.InFlight() == 0);  // 异常路径也必须递减在途计数

    // 总线状态不变，仍可继续服务
    CHECK(bus.RegisteredCount() == 1);
    const auto again = bus.Dispatch(fx::MovePlayerCommand{}, CommandContext{});
    CHECK(!again.HasValue());
    CHECK(again.Err().Code() == ErrorCode::INTERNAL_ERROR);
}

// ---- §16 QueryBus：Ask 与错误传播 ----------------------------------------

void TestQueryAskAndErrorPropagation() {
    QueryBus bus;
    const fx::Position state{7.0f, 8.0f};

    CHECK(EXPECT_OK(bus.RegisterFn<fx::GetPositionQuery>(
        [&state](const fx::GetPositionQuery& q, const QueryContext&) -> Result<fx::Position> {
            if (q.player_id == 0) {
                return Result<fx::Position>::Fail(
                    Error(ErrorCode::INVALID_ARGUMENT, "bad player", domain::kCore));
            }
            return Result<fx::Position>::Ok(state);
        })));
    CHECK(bus.RegisteredCount() == 1);

    fx::GetPositionQuery query;
    query.player_id = 1;
    const fx::Position p = EXPECT_OK(bus.Ask(query, QueryContext{}));
    CHECK(p.x == 7.0f);
    CHECK(p.y == 8.0f);

    // 错误原样传播（不吞、不改码）
    fx::GetPositionQuery bad;
    bad.player_id = 0;
    const auto err = bus.Ask(bad, QueryContext{});
    CHECK(!err.HasValue());
    CHECK(err.Err().Code() == ErrorCode::INVALID_ARGUMENT);

    // 未注册类型 → NOT_FOUND
    const auto missing = bus.Ask(fx::UnregisteredQuery{}, QueryContext{});
    CHECK(!missing.HasValue());
    CHECK(missing.Err().Code() == ErrorCode::NOT_FOUND);
}

// ---- §17 Query 无副作用：只读区间内的写入必须被探测到 --------------------

void TestQueryNoSideEffect() {
    QueryBus bus;
    SideEffectProbe probe;

    CHECK(EXPECT_OK(bus.RegisterFn<fx::GetPositionQuery>(
        [&probe](const fx::GetPositionQuery&, const QueryContext&) -> Result<fx::Position> {
            probe.Write();  // 违规：Query handler 内写状态
            return Result<fx::Position>::Ok(fx::Position{});
        })));

    // Ask 之外写入：只是普通写，不算违规
    probe.Write();
    CHECK(probe.Violations() == 0);
    CHECK(probe.Writes() == 1);

    fx::GetPositionQuery query;
    query.player_id = 1;
    (void)EXPECT_OK(bus.Ask(query, QueryContext{}));
    CHECK(probe.Violations() == 1);  // 只读区间内的写被捕获（§17 判定失败的依据）
    CHECK(probe.Writes() == 2);

    // 区间退出后写入不再算违规
    probe.Write();
    CHECK(probe.Violations() == 1);
    CHECK(probe.Writes() == 3);
    CHECK(!mmo::core::detail::QueryReadOnlyActive());
    CHECK(mmo::core::detail::QueryReadOnlyDepth() == 0);
}

// ---- §16 EventBus：多播按注册顺序 / 异步语义 ------------------------------

void TestEventMulticastOrder() {
    EventBus bus;
    std::vector<int> order;

    (void)EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>(
        [&order](const fx::PlayerMovedEvent&) { order.push_back(1); }));
    (void)EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>(
        [&order](const fx::PlayerMovedEvent&) { order.push_back(2); }));
    CHECK(bus.SubscriberCount() == 2);

    fx::PlayerMovedEvent ev;
    ev.player_id = 1001;
    ev.x = 10;
    ev.y = 20;
    CHECK(EXPECT_OK(bus.Publish(ev)));
    CHECK(bus.QueueDepth() == 1);
    CHECK(order.empty());  // Publish 只入队，不立即派发

    const std::size_t remaining = EXPECT_OK(bus.Drain(64, DurationMs(2)));
    CHECK(remaining == 0);
    CHECK(order.size() == 2);
    CHECK(order[0] == 1);  // 按注册顺序派发
    CHECK(order[1] == 2);
}

// ---- §16 退订生效 + 幂等 --------------------------------------------------

void TestEventUnsubscribeIdempotent() {
    EventBus bus;
    int hits = 0;
    const EventBus::SubId id = EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>(
        [&hits](const fx::PlayerMovedEvent&) { ++hits; }));
    CHECK(id != 0);

    CHECK(EXPECT_OK(bus.PublishImmediate(fx::PlayerMovedEvent{})));
    CHECK(hits == 1);

    CHECK(EXPECT_OK(bus.Unsubscribe(id)));
    CHECK(bus.SubscriberCount() == 0);
    CHECK(EXPECT_OK(bus.PublishImmediate(fx::PlayerMovedEvent{})));
    CHECK(hits == 1);  // 退订后不再收到

    CHECK(EXPECT_OK(bus.Unsubscribe(id)));        // 幂等：重复退订仍成功
    CHECK(EXPECT_OK(bus.Unsubscribe(999999)));    // 非法 ID 同样成功
}

// ---- §19 订阅者异常隔离：一个坏订阅者不得拖垮总线 ------------------------

void TestEventSubscriberExceptionIsolation() {
    EventBus bus;
    int second_hits = 0;

    (void)EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>([](const fx::PlayerMovedEvent&) {
        throw std::runtime_error("bad subscriber");
    }));
    (void)EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>(
        [&second_hits](const fx::PlayerMovedEvent&) { ++second_hits; }));

    CHECK(EXPECT_OK(bus.PublishImmediate(fx::PlayerMovedEvent{})));
    CHECK(second_hits == 1);              // 后注册的订阅者照常收到
    CHECK(bus.SubscriberErrors() == 1);   // 异常被记指标，不外泄

    CHECK(EXPECT_OK(bus.PublishImmediate(fx::PlayerMovedEvent{})));
    CHECK(second_hits == 2);
    CHECK(bus.SubscriberErrors() == 2);
}

// ---- §19 Drain 上限与预算耗尽：返回剩余数量，不阻塞 ----------------------

void TestEventDrainBudgetAndRemaining() {
    EventBusOptions opt;
    opt.queue_capacity = 1024;
    EventBus bus(opt);

    int hits = 0;
    (void)EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>(
        [&hits](const fx::PlayerMovedEvent&) { ++hits; }));

    for (int i = 0; i < 100; ++i) {
        CHECK(EXPECT_OK(bus.Publish(fx::PlayerMovedEvent{})));
    }
    CHECK(bus.QueueDepth() == 100);

    // max_events 上限：精确只处理 10 个
    const std::size_t remaining = EXPECT_OK(bus.Drain(10, DurationMs(1000)));
    CHECK(hits == 10);
    CHECK(remaining == 90);  // 返回值 = 队列中剩余未处理事件数

    // 继续排空
    CHECK(EXPECT_OK(bus.Drain(1000, DurationMs(1000))) == 0);
    CHECK(hits == 100);

    // 预算耗尽（budget=0）：允许处理极少量后立即返回，且必须留有剩余
    for (int i = 0; i < 100; ++i) {
        CHECK(EXPECT_OK(bus.Publish(fx::PlayerMovedEvent{})));
    }
    const std::size_t left = EXPECT_OK(bus.Drain(1000, DurationMs(0)));
    CHECK(left > 0);
    CHECK(hits < 200);
}

// ---- §19 背压：非关键丢弃计数 / 关键事件返回 BUSY 且不丢 -----------------

void TestEventBackpressure() {
    EventBusOptions opt;
    opt.queue_capacity = 4;  // 容量 4（2 的幂，不取整）
    EventBus bus(opt);
    CHECK(bus.Capacity() == 4);

    for (int i = 0; i < 4; ++i) {
        CHECK(EXPECT_OK(bus.Publish(fx::PlayerMovedEvent{})));
    }
    CHECK(bus.QueueDepth() == 4);

    // 非关键事件：丢弃并计数，Publish 仍返回 OK（降级不阻塞热路径）
    CHECK(EXPECT_OK(bus.Publish(fx::PlayerMovedEvent{})));
    CHECK(bus.DroppedCount() == 1);
    CHECK(bus.QueueDepth() == 4);

    // 关键事件（经济类）：返回 BUSY，绝不丢弃
    const auto critical = bus.Publish(fx::EconomyEvent{1, 100, 7});
    CHECK(!critical.HasValue());
    CHECK(critical.Err().Code() == ErrorCode::BUSY);
    CHECK(bus.DroppedCount() == 1);  // 关键事件不计入丢弃计数
    CHECK(bus.QueueDepth() == 4);

    // 排空后关键事件可正常入队
    CHECK(EXPECT_OK(bus.Drain(4, DurationMs(10))) == 0);
    CHECK(EXPECT_OK(bus.Publish(fx::EconomyEvent{1, 100, 7})));
    CHECK(bus.DroppedCount() == 1);
}

// ---- §16 大事件（>32B）走堆路径，行为与内联路径一致 ----------------------

void TestBigEventHeapPath() {
    EventBus bus;
    bool matched = false;
    (void)EXPECT_OK(bus.Subscribe<fx::BigEvent>(
        [&matched](const fx::BigEvent& e) { matched = (e.player_id == 7 && e.payload[3] == 42); }));

    fx::BigEvent big;
    big.player_id = 7;
    big.payload[3] = 42;
    CHECK(EXPECT_OK(bus.Publish(big)));
    CHECK(EXPECT_OK(bus.Drain(8, DurationMs(10))) == 0);
    CHECK(matched);  // 堆路径事件的负载未被破坏
}

// ---- §17 跨线程：Worker 投递 Command，trace 上下文不丢失 ------------------

void TestCrossThreadTracePropagation() {
    CommandBus bus;
    const TraceID expected = NewTraceID();
    std::atomic<int> seen{0};

    CHECK(EXPECT_OK(bus.RegisterFn<fx::MovePlayerCommand>(
        [&seen, expected](const fx::MovePlayerCommand&, const CommandContext& ctx) -> Result<fx::Position> {
            if (ctx.trace_id == expected) {
                seen.fetch_add(1, std::memory_order_relaxed);
            }
            return Result<fx::Position>::Ok(fx::Position{});
        })));

    // 总线不切换线程：handler 在调用者线程执行，但上下文必须原样透传。
    std::thread worker([&bus, expected] {
        for (int i = 0; i < 2000; ++i) {
            CommandContext ctx;
            ctx.trace_id = expected;
            ctx.player_id = 5;
            fx::MovePlayerCommand cmd;
            cmd.player_id = 5;
            (void)bus.Dispatch(cmd, ctx);
        }
    });
    worker.join();

    CHECK(seen.load() == 2000);
    CHECK(bus.InFlight() == 0);
}

// ---- §17 混合负载：多生产者 Publish + 宿主 Drain，无死锁、无丢失 ---------

void TestMixedLoadNoDeadlock() {
    // 容量必须 >= 总事件数（4 × 20000 = 80000）：本节要断言「零丢弃」，
    // 若容量不足，背压丢弃会让断言变成对调度抖动的考验，失去确定性。
    EventBusOptions opt;
    opt.queue_capacity = 1u << 17;
    EventBus bus(opt);
    CommandBus commands;

    std::atomic<long long> dispatched{0};
    std::atomic<long long> received{0};

    CHECK(EXPECT_OK(commands.RegisterFn<fx::MovePlayerCommand>(
        [&dispatched](const fx::MovePlayerCommand&, const CommandContext&) -> Result<fx::Position> {
            dispatched.fetch_add(1, std::memory_order_relaxed);
            return Result<fx::Position>::Ok(fx::Position{});
        })));
    (void)EXPECT_OK(bus.Subscribe<fx::PlayerMovedEvent>(
        [&received](const fx::PlayerMovedEvent&) { received.fetch_add(1, std::memory_order_relaxed); }));

    constexpr int kWorkers = 4;
    constexpr int kPerWorker = 20000;  // 合计 8e4 次混合操作

    std::atomic<bool> stop{false};
    // 宿主线程（模拟 SimulationThread）持续 Drain：验证 Publish/Drain 并发无死锁
    std::thread drainer([&bus, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)bus.Drain(4096, DurationMs(1));
        }
        std::size_t rem = 0;
        do {
            const auto r = bus.Drain(4096, DurationMs(2));
            rem = r.HasValue() ? r.Value() : 0;
        } while (rem > 0);
    });

    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (int t = 0; t < kWorkers; ++t) {
        workers.emplace_back([&bus, &commands] {
            for (int i = 0; i < kPerWorker; ++i) {
                (void)commands.Dispatch(fx::MovePlayerCommand{}, CommandContext{});
                (void)bus.Publish(fx::PlayerMovedEvent{});
            }
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    stop.store(true, std::memory_order_relaxed);
    drainer.join();

    CHECK(dispatched.load() == kWorkers * kPerWorker);
    CHECK(received.load() == kWorkers * kPerWorker);  // 队列容量充足 → 零丢弃
    CHECK(bus.DroppedCount() == 0);
    CHECK(bus.QueueDepth() == 0);

    // 关键事件零丢弃（§21）：容量充足时全部成功入队
    for (int i = 0; i < 1000; ++i) {
        CHECK(bus.Publish(fx::EconomyEvent{1, 1, 1}).HasValue());
    }
    CHECK(bus.DroppedCount() == 0);
}

}  // namespace

int main() {
    tprint::Line("=== Core Bus (TASK-007) ===\n");

    TestCommandRegisterDispatch();
    TestCommandDuplicateRegisterNotOverwrite();
    TestCommandNotFound();
    TestCommandHandlerThrows();
    TestQueryAskAndErrorPropagation();
    TestQueryNoSideEffect();
    TestEventMulticastOrder();
    TestEventUnsubscribeIdempotent();
    TestEventSubscriberExceptionIsolation();
    TestEventDrainBudgetAndRemaining();
    TestEventBackpressure();
    TestBigEventHeapPath();
    TestCrossThreadTracePropagation();
    TestMixedLoadNoDeadlock();

    if (g_failures == 0) {
        tprint::Line("ALL CORE_BUS TESTS PASSED\n");
        return 0;
    }
    tprint::ErrorFmt("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
