// engine/core/tests/mem_bench.cpp — TASK-004 Memory 三件套 Benchmark
//
// 输出机器可读 key=value 到 stdout 与 bench/core_mem.txt（供验收脚本 assert_metric 解析）。
// 阈值（§22）：pool_acquire_release_ns <= 20；文档参考值 arena_push_ns < 3、mempool_alloc_ns < 15。
//
// 计时口径：用 MonotonicClock 自测。
// 反优化：每次分配的指针都落到 volatile sink，防止 -O3 把 acquire/release 配对消除。

#include "mmo/core/memory/arena.h"
#include "mmo/core/memory/memory_pool.h"
#include "mmo/core/memory/object_pool.h"
#include "mmo/core/time/clock.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "test_print.h"

namespace {

namespace tprint = ::mmo::core::test;
using mmo::core::Arena;
using mmo::core::MemoryPool;
using mmo::core::MonotonicClock;
using mmo::core::ObjectPool;
using mmo::core::SteadyNs;

double NsPerOp(SteadyNs total_ns, std::size_t iterations) {
    return static_cast<double>(total_ns) / static_cast<double>(iterations);
}

volatile std::int64_t g_sink = 0;

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 10000000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ops") == 0 && (i + 1) < argc) {
            ops = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    if (ops == 0) {
        ops = 10000000;
    }

    // ---- 1) ObjectPool：Acquire + Release 一轮回 ----
    double pool_acquire_release_ns = 0.0;
    {
        // prewarm 到 ops 个槽位，保证测量窗口里零扩容、零 OS 分配。
        ObjectPool<std::size_t, 4096> pool(ops);
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < ops; ++i) {
            std::size_t* p = pool.Acquire();
            pool.Release(p);
            g_sink = reinterpret_cast<std::int64_t>(p) + static_cast<std::int64_t>(i & 0xFF);
        }
        pool_acquire_release_ns = NsPerOp(MonotonicClock::Now() - start, ops);
    }

    // ---- 2) Arena：Push 单次（首块足够大，测量窗口内零扩容） ----
    double arena_push_ns = 0.0;
    {
        // 10M 次 * 8 字节 = 80MB，首块给 128MB，全程不 AddBlock。
        Arena arena(128ULL * 1024ULL * 1024ULL);
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < ops; ++i) {
            void* p = arena.Push(8, 8);
            g_sink = reinterpret_cast<std::int64_t>(p) + static_cast<std::int64_t>(i & 0xFF);
        }
        arena_push_ns = NsPerOp(MonotonicClock::Now() - start, ops);
    }

    // ---- 3) MemoryPool：Allocate 单次（拥有者线程路径，无锁） ----
    double mempool_alloc_ns = 0.0;
    {
        MemoryPool pool(64, 1024);
        const SteadyNs start = MonotonicClock::Now();
        for (std::size_t i = 0; i < ops; ++i) {
            void* p = pool.Allocate(32);
            pool.Deallocate(p, 32);
            g_sink = reinterpret_cast<std::int64_t>(p) + static_cast<std::int64_t>(i & 0xFF);
        }
        mempool_alloc_ns = NsPerOp(MonotonicClock::Now() - start, ops);
    }

    char buf[1024];
    const int written = std::snprintf(
        buf, sizeof(buf),
        "pool_acquire_release_ns=%.3f\n"
        "arena_push_ns=%.3f\n"
        "mempool_alloc_ns=%.3f\n"
        "ops=%zu\n",
        pool_acquire_release_ns, arena_push_ns, mempool_alloc_ns, ops);
    if (written > 0) {
        tprint::Write(buf, static_cast<std::size_t>(written), stdout);
    }

    std::FILE* file = std::fopen("bench/core_mem.txt", "w");
    if (file != nullptr) {
        std::fputs(buf, file);
        std::fclose(file);
    } else {
        tprint::Error("WARN: cannot write bench/core_mem.txt\n");
        return 1;
    }
    return 0;
}
