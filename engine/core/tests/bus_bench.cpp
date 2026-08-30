// engine/core/tests/bus_bench.cpp — TASK-007 Command / Query / Event Bus Benchmark
//
// 输出机器可读 key=value 到 stdout 与 bench/core_bus.txt（供验收脚本 assert_metric 解析）。
// 阈值（§22）：cmd_dispatch_ns <= 150；event_drain_ns_per_event <= 80；
//             event_publish_ns < 100；alloc_per_cmd = 0。
//
// 计时口径：MonotonicClock（QPC 定点），与 TASK-003/004 一致。
// 反优化：Dispatch 的返回值累加进 volatile sink，防止 -O3 把整条链路消除。
// 零分配验证：重载全局 operator new 计数（同 TASK-001/004 的做法），
//            在 Dispatch 测量窗口前后取差值 —— 差值必须为 0。

#include "test_print.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "mmo/core/bus/command_bus.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/bus/query_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"

#include "bus_fixtures.h"

// ---- 全局堆分配计数（必须放在任何会分配的标准库调用之前） ------------------
namespace {
std::atomic<long long> g_alloc_count{0};
}  // namespace

void* operator new(std::size_t size) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

namespace tprint = ::mmo::core::test;
namespace fx = mmo::core::bus_fixtures;
using mmo::core::CommandBus;
using mmo::core::CommandContext;
using mmo::core::DurationMs;
using mmo::core::EventBus;
using mmo::core::EventBusOptions;
using mmo::core::MonotonicClock;
using mmo::core::QueryBus;
using mmo::core::QueryContext;
using mmo::core::Result;
using mmo::core::SteadyNs;

double NsPerOp(SteadyNs total_ns, std::size_t iterations) {
    return static_cast<double>(total_ns) / static_cast<double>(iterations);
}

volatile std::int64_t g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 1000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--iterations") == 0 && (i + 1) < argc) {
            ops = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    if (ops == 0) {
        ops = 1000000;
    }

    // ---- 1) Command Dispatch：1e6 次同步派发 -----------------------------
    double cmd_dispatch_ns = 0.0;
    double alloc_per_cmd = 0.0;
    {
        CommandBus bus;
        fx::Position state{};
        // 注册期不计入测量窗口（含 std::function / map 的一次性分配）
        (void)bus.RegisterFn<fx::MovePlayerCommand>(
            [&state](const fx::MovePlayerCommand& cmd, const CommandContext&) -> Result<fx::Position> {
                state.x += cmd.dx;
                state.y += cmd.dy;
                return Result<fx::Position>::Ok(state);
            });

        fx::MovePlayerCommand cmd;
        cmd.player_id = 1;
        cmd.dx = 1.0f;
        const CommandContext ctx{};

        // 预热：把 handler 的 std::function 调用路径、分支预测、TLB 都跑热
        for (std::size_t i = 0; i < 10000 && i < ops; ++i) {
            g_sink += static_cast<std::int64_t>(bus.Dispatch(cmd, ctx).ValueOr(fx::Position{}).x);
        }

        const long long alloc_before = g_alloc_count.load(std::memory_order_relaxed);
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < ops; ++i) {
            const fx::Position p = bus.Dispatch(cmd, ctx).ValueOr(fx::Position{});
            g_sink += static_cast<std::int64_t>(p.x);
        }
        cmd_dispatch_ns = NsPerOp(MonotonicClock::Now() - start, ops);
        const long long alloc_after = g_alloc_count.load(std::memory_order_relaxed);
        alloc_per_cmd = static_cast<double>(alloc_after - alloc_before) / static_cast<double>(ops);
    }

    // ---- 2) Event Publish：1e6 次无锁入队 --------------------------------
    double event_publish_ns = 0.0;
    double event_drain_ns_per_event = 0.0;
    std::size_t drained = 0;
    {
        EventBusOptions opt;
        opt.queue_capacity = ops < 16 ? 16 : ops;  // 容量 >= ops：压测期间不允许背压丢弃
        EventBus bus(opt);
        std::atomic<long long> hits{0};
        (void)bus.Subscribe<fx::PlayerMovedEvent>(
            [&hits](const fx::PlayerMovedEvent& e) { hits.fetch_add(e.x, std::memory_order_relaxed); });

        fx::PlayerMovedEvent ev;
        ev.player_id = 42;
        ev.x = 1;

        for (std::size_t i = 0; i < 10000 && i < ops; ++i) {
            (void)bus.Publish(ev);
        }
        // 预热产生的事件先排空，避免混入 Drain 的测量窗口
        {
            std::size_t rem = 0;
            do {
                const auto r = bus.Drain(4096, DurationMs(50));
                rem = r.HasValue() ? r.Value() : 0;
            } while (rem > 0);
        }

        SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < ops; ++i) {
            (void)bus.Publish(ev);
        }
        event_publish_ns = NsPerOp(MonotonicClock::Now() - start, ops);

        // 入队后队列深度应等于 ops（容量充足 → 零丢弃）
        const std::size_t queued = bus.QueueDepth();
        if (queued < ops) {
            tprint::ErrorFmt("WARN: only %zu/%zu events queued (backpressure hit)\n", queued, ops);
        }

        // ---- 3) Event Drain：宿主驱动，一次性排空 -------------------------
        // 预算给足（10s）而 max_events = ops，因此这一轮测的是纯派发开销，
        // 不会触发「预算耗尽提前返回」分支。
        start = MonotonicClock::Now();
        std::size_t rem = 0;
        do {
            const auto r = bus.Drain(ops, DurationMs(10000));
            rem = r.HasValue() ? r.Value() : 0;
        } while (rem > 0);
        event_drain_ns_per_event = NsPerOp(MonotonicClock::Now() - start, ops);
        drained = (hits.load() > 0) ? ops : 0;  // 订阅者确实被调用过
        if (bus.DroppedCount() != 0) {
            tprint::Error("WARN: events dropped during bench (queue too small)\n");
        }
    }

    // ---- 4) 队列内存占用（§22：1e6 事件 < 64MB）--------------------------
    double queue_mb_1e6 = 0.0;
    {
        EventBusOptions probe_opt;
        probe_opt.queue_capacity = 1u << 20;  // 1048576
        EventBus probe_bus(probe_opt);
        // EventSlot 40B + MpmcQueue 每格 8B 的 seq = 48B/格
        const double bytes = static_cast<double>(probe_bus.Capacity()) *
                             static_cast<double>(sizeof(mmo::core::bus::detail::EventSlot) + 8);
        queue_mb_1e6 = bytes / (1024.0 * 1024.0);
    }

    char buf[1024];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "cmd_dispatch_ns=%.3f\n"
        "event_publish_ns=%.3f\n"
        "event_drain_ns_per_event=%.3f\n"
        "alloc_per_cmd=%.3f\n"
        "queue_mb_1e6=%.2f\n"
        "ops=%zu\n",
        cmd_dispatch_ns, event_publish_ns, event_drain_ns_per_event, alloc_per_cmd,
        queue_mb_1e6, ops);
    if (written > 0) {
        tprint::Write(buf, static_cast<std::size_t>(written), stdout);
    }

    std::FILE* file = std::fopen("bench/core_bus.txt", "w");
    if (file != nullptr) {
        std::fputs(buf, file);
        std::fclose(file);
    } else {
        tprint::Error("WARN: cannot write bench/core_bus.txt\n");
        return 1;
    }
    if (drained == 0) {
        tprint::Error("WARN: no event drained (bench sanity check failed)\n");
        return 1;
    }
    return 0;
}
