// CAMI Event Bus 吞吐基准
// 验收目标: 总线吞吐 >= 1,000,000 msg/s (1 Mmsg/s)
//
// 两个基准:
//   1) bench_mpmc : 裸 mpmc_queue 吞吐（生产者/消费者竞争同一无锁队列），
//      反映队列本身的极限能力。
//   2) bench_bus  : EventBus 端到端（publish + drain + no-op handler），
//      反映事件总线作为"发布/订阅"抽象后的真实吞吐。
//
// 运行: 见 benchmark/CMakeLists.txt 的 event_bus_bench target。
//   MINGW64 终端: cmake --build build -j && ./build/bin/event_bus_bench

#include "common/event_bus/mpmc_queue.h"
#include "common/event_bus/event_bus.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

double steady_sec() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

// 基准1: 裸 MPMC 队列吞吐
void bench_mpmc(std::size_t producers, std::size_t consumers,
                std::size_t capacity, double run_sec) {
    cami::common::mpmc_queue<uint64_t> q(capacity);
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool> stop{false};

    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < producers; ++i) {
        ts.emplace_back([&] {
            uint64_t x = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (q.enqueue(x)) ++x;
            }
        });
    }
    for (std::size_t i = 0; i < consumers; ++i) {
        ts.emplace_back([&] {
            uint64_t v = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                if (q.dequeue(v)) consumed.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    const double t0 = steady_sec();
    std::this_thread::sleep_for(std::chrono::duration<double>(run_sec));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : ts) t.join();
    const double elapsed = steady_sec() - t0;

    const double mps = static_cast<double>(consumed.load()) / elapsed / 1e6;
    std::printf("[bench] MPMC raw queue   P=%zu C=%zu cap=%zu : %.2f Mmsg/s (consumed=%llu, %.2fs)\n",
                producers, consumers, capacity, mps,
                static_cast<unsigned long long>(consumed.load()), elapsed);
}

// 基准2: EventBus 端到端（publish + drain + no-op handler）
void bench_bus(std::size_t producers, std::size_t consumers,
               std::size_t capacity, double run_sec) {
    cami::common::EventBus<uint64_t> bus(capacity);
    auto* ch = bus.channel("events");
    std::atomic<uint64_t> handled{0};
    ch->subscribe([&](uint64_t) { handled.fetch_add(1, std::memory_order_relaxed); });

    std::atomic<bool> stop{false};
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < producers; ++i) {
        ts.emplace_back([&] {
            uint64_t x = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                ch->publish(x);
                ++x;
            }
        });
    }
    for (std::size_t i = 0; i < consumers; ++i) {
        ts.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                ch->drain(1024);
            }
        });
    }

    const double t0 = steady_sec();
    std::this_thread::sleep_for(std::chrono::duration<double>(run_sec));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : ts) t.join();
    const double elapsed = steady_sec() - t0;

    const double mps = static_cast<double>(handled.load()) / elapsed / 1e6;
    std::printf("[bench] EventBus e2e      P=%zu C=%zu cap=%zu : %.2f Mmsg/s (handled=%llu, %.2fs)\n",
                producers, consumers, capacity, mps,
                static_cast<unsigned long long>(handled.load()), elapsed);
}

}  // namespace

int main() {
    std::printf("=== CAMI Event Bus Throughput Benchmark ===\n");
    std::printf("acceptance target: >= 1.00 Mmsg/s\n\n");

    // 单生产者单消费者：最小竞争，看队列本身延迟/吞吐上限
    bench_mpmc(1, 1, 1u << 20, 2.0);
    // 多生产者多消费者：真实并发场景
    bench_mpmc(4, 4, 1u << 20, 2.0);

    bench_bus(1, 1, 1u << 20, 2.0);
    bench_bus(4, 4, 1u << 20, 2.0);

    std::printf("\n=== done ===\n");
    return 0;
}
