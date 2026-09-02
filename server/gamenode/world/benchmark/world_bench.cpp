// server/gamenode/world/benchmark/world_bench.cpp — TASK-020 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/world.txt（验收脚本 assert_metric 解析）：
//   instances / duration_ms / instance_create_us / instance_destroy_us /
//   tick_us_per_100_instances / mem_bytes_per_instance
//
// 内存口径：替换全局 operator new/delete，按 _msize 统计**净堆增量**（同 quest/role/ai bench）。
// 单实例元数据内存 = 创建 N 个 Pending 实例的净堆增量 / N（§22 < 4KB）。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// ---- 全局堆占用统计（必须定义在 operator new/delete 之前） ----
namespace {
std::int64_t g_heap_bytes = 0;
}  // namespace

void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    g_heap_bytes += static_cast<std::int64_t>(_msize(p));
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (p != nullptr) {
        g_heap_bytes -= static_cast<std::int64_t>(_msize(p));
        std::free(p);
    }
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { operator delete(p); }

#include <chrono>
#include <filesystem>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/movement/movement_system.h"
#include "mmo/game/ai/ai_system.h"
#include "mmo/game/world/instance_manager.h"
#include "mmo/game/world/world_config.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::world;
using namespace mmo::game::ai;
using namespace mmo::game::movement;

using core::MonotonicClock;
using core::SteadyNs;
using core::SteadyTime;

using core::test::ErrorFmt;
using core::test::LineFmt;

constexpr const char* kWorldDir = "config/gameplay/world";

int g_bench_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_bench_fail;                                                  \
        }                                                                    \
    } while (0)

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena;
    EntityManager mgr;
    std::unique_ptr<aoi::IAoi> aoi_owner;
    aoi::IAoi& aoi;
    movement::MovementSystem movement;
    AiSystem ai;
    SceneManager scenes;
    InstanceManager instances;

    Harness()
        : arena(8 * 1024 * 1024),
          mgr(&bus),
          aoi_owner(aoi::CreateDynamicGridAoi(aoi::AoiConfig{})),
          aoi(*aoi_owner),
          movement(movement::MovementConfig{}, &aoi),
          ai(mgr, movement, aoi, scheduler),
          scenes(mgr, bus, scheduler, arena),
          instances(scenes, ai, mgr, bus, scheduler, arena, 1) {
        movement.BindAoi(aoi);
    }
};

void Run(std::size_t N, std::int64_t duration_ms, const char* out_path) {
    Harness h;
    CHECK(h.instances.LoadConfig(kWorldDir).HasValue());
    const SteadyTime base = MonotonicClock::Point();
    (void)h.instances.Tick(base);
    h.mgr.FlushDeferred();
    const std::int64_t heap_before = g_heap_bytes;

    // ---- 创建耗时（Create 仅分配 + 入表，Pending） ----
    std::vector<InstanceId> ids;
    ids.reserve(N);
    const SteadyNs tc0 = MonotonicClock::Now();
    for (std::size_t i = 0; i < N; ++i) {
        auto c = h.instances.Create(1001, std::vector<PlayerId>{}, 0);
        CHECK(c.HasValue());
        ids.push_back(c.Value());
    }
    const SteadyNs tc1 = MonotonicClock::Now();
    const double instance_create_us = static_cast<double>(tc1 - tc0) / static_cast<double>(N) / 1000.0;

    const std::int64_t heap_after_create = g_heap_bytes;
    const double mem_bytes_per_instance =
        static_cast<double>(heap_after_create - heap_before) / static_cast<double>(N);

    // ---- Start（重活：建 Scene + 生成怪物，不计时） ----
    for (InstanceId id : ids) CHECK(h.instances.Start(id, 0).HasValue());

    // ---- Tick 遍历耗时（单 Tick 跨 N 个实例） ----
    const SteadyNs tk0 = MonotonicClock::Now();
    (void)h.instances.Tick(base + core::DurationMs(1000));  // 1s 内仍 Running，仅测遍历开销
    const SteadyNs tk1 = MonotonicClock::Now();
    const double tick_us_per_100_instances =
        static_cast<double>(tk1 - tk0) * 100.0 / static_cast<double>(N) / 1000.0;

    // ---- 销毁耗时（Destroy + Despawn 怪物） ----
    const SteadyNs td0 = MonotonicClock::Now();
    for (InstanceId id : ids) CHECK(h.instances.Destroy(id, 0).HasValue());
    const SteadyNs td1 = MonotonicClock::Now();
    const double instance_destroy_us = static_cast<double>(td1 - td0) / static_cast<double>(N) / 1000.0;
    h.mgr.FlushDeferred();

    std::FILE* fp = std::fopen(out_path, "w");
    if (fp == nullptr) { ErrorFmt("FAIL: cannot open %s\n", out_path); std::exit(1); }
    std::fprintf(fp,
                 "instances=%zu\n"
                 "duration_ms=%lld\n"
                 "instance_create_us=%.4f\n"
                 "instance_destroy_us=%.4f\n"
                 "tick_us_per_100_instances=%.4f\n"
                 "mem_bytes_per_instance=%.4f\n",
                 N, static_cast<long long>(duration_ms), instance_create_us,
                 instance_destroy_us, tick_us_per_100_instances, mem_bytes_per_instance);
    std::fclose(fp);

    LineFmt("instances=%zu duration_ms=%lld instance_create_us=%.4f "
             "instance_destroy_us=%.4f tick_us_per_100_instances=%.4f "
             "mem_bytes_per_instance=%.4f\n",
             N, static_cast<long long>(duration_ms), instance_create_us,
             instance_destroy_us, tick_us_per_100_instances, mem_bytes_per_instance);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t N = 100;
    std::int64_t duration_ms = 600;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--instances" && i + 1 < argc) N = static_cast<std::size_t>(std::atoll(argv[++i]));
        else if (a == "--duration" && i + 1 < argc) duration_ms = static_cast<std::int64_t>(std::atoll(argv[++i]));
    }
    LineFmt("== TASK-020 world_bench ==\n");
    Run(N, duration_ms, "bench/world.txt");
    return g_bench_fail == 0 ? 0 : 1;
}
