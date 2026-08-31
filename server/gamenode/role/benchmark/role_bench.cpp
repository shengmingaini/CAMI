// server/gamenode/role/benchmark/role_bench.cpp — TASK-016 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/role.txt（验收脚本 assert_metric 解析）：
//   characters / recompute_ns / add_exp_ns / modify_hp_ns / mem_bytes_per_character /
//   save_enqueue_ns
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
//
// 内存口径：替换全局 operator new/delete，按 `_msize` 统计**净堆占用**增量。
// 之所以用净值而非「累计分配」：LoadOrCreate 内部有 std::to_string 之类的临时分配，
// 累计口径会把它们算成每角色常驻成本，虚高且不可复现。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

// ---- 全局堆占用统计（必须定义在 operator new/delete 之前） ----
namespace {
std::int64_t g_heap_bytes = 0;
std::int64_t g_heap_ops = 0;
}  // namespace

void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (p == nullptr) throw std::bad_alloc();
    g_heap_bytes += static_cast<std::int64_t>(_msize(p));
    ++g_heap_ops;
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

#include <algorithm>
#include <cstddef>
#include <limits>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/config/config_manager.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;             // SceneContext / EntityManager / PlayerId
using namespace mmo::game::role;       // RoleSystem / Character / ExpCurve ...

using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;

using core::MonotonicClock;
using core::SteadyNs;

constexpr const char* kExpCurvePath = "config/gameplay/exp_curve.json";
constexpr std::uint64_t kScene = 7;

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
    EntityManager mgr{&bus};
    SceneContext ctx;

    Harness()
        : ctx(kScene, SceneType::World, 1, MonotonicClock::Point(), 0, mgr, bus, scheduler,
              arena) {}
};

/// 百分位（bench 侧排序，不进热路径）。
double Percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t idx = static_cast<std::size_t>((v.size() - 1) * q);
    return v[idx];
}

void Run(std::size_t n, std::size_t iters, const char* out_path) {
    Harness h;
    InMemoryPersistenceAdapter sink;

    // 曲线从配置文件加载（§8 配置化，禁止硬编码）；必须在内存基线之前完成。
    core::ConfigManager::ResetForTest();
    auto curve_r = ExpCurve::LoadFromFile(kExpCurvePath);
    if (!curve_r.HasValue()) {
        ErrorFmt("FAIL: cannot load %s\n", kExpCurvePath);
        std::exit(1);
    }
    RoleSystem sys(sink, std::move(curve_r).Value());
    sys.BindEventBus(h.bus);
    sys.Reserve(n);

    std::vector<CharacterId> ids;
    ids.reserve(n);

    // ---- 内存：创建 n 个角色的净堆增量 ----
    const std::int64_t heap_before = g_heap_bytes;
    for (std::size_t i = 0; i < n; ++i) {
        const CharacterId cid = 10000 + i;
        auto c = sys.LoadOrCreate(static_cast<PlayerId>(i + 1), cid, h.ctx);
        if (!c.HasValue()) {
            ErrorFmt("FAIL: LoadOrCreate failed at %zu\n", i);
            std::exit(1);
        }
        ids.push_back(cid);
    }
    const double mem_per_char =
        static_cast<double>(g_heap_bytes - heap_before) / static_cast<double>(n);

    // 缓存 Character* 指针：Reserve 后容器不会 rehash，指针稳定。
    // 用途：add_exp 循环里把 exp 归零时避免再走一次哈希查找，否则测到的是
    // 「AddExp + Find」的合计，而非 AddExp 本身。
    std::vector<Character*> ptrs;
    ptrs.reserve(n);
    for (CharacterId cid : ids) ptrs.push_back(sys.Find(cid));

    // 计时口径：单调钟在 Windows 上的分辨率约 100ns，单次操作（几十 ns）无法直接采样。
    // 因此改为「批量计时 + 除法」：每轮遍历全部 n 个角色各操作一次，累计 R 轮后
    // 取 总时间 / (n * R)。每轮样本数 = n，R 轮样本用于取中位数，规避调度抖动。
    const std::size_t rounds = (iters + n - 1) / n;  // 总操作数约 iters

    std::vector<double> rc_samples;
    std::vector<double> exp_samples;
    std::vector<double> hp_samples;
    std::vector<double> save_samples;
    rc_samples.reserve(rounds);
    exp_samples.reserve(rounds);
    hp_samples.reserve(rounds);
    save_samples.reserve(rounds);

    for (std::size_t r = 0; r < rounds; ++r) {
        // 1) RecomputeAttributes
        {
            const SteadyNs t0 = MonotonicClock::Now();
            for (std::size_t i = 0; i < n; ++i) (void)sys.RecomputeAttributes(ids[i]);
            const SteadyNs t1 = MonotonicClock::Now();
            rc_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(n));
        }
        // 2) AddExp 稳态：每次后把 exp 归零，隔离升级路径（升级成本另计）
        {
            const SteadyNs t0 = MonotonicClock::Now();
            for (std::size_t i = 0; i < n; ++i) {
                (void)sys.AddExp(ids[i], 1, core::NewTraceID());
                ptrs[i]->exp = 0;
            }
            const SteadyNs t1 = MonotonicClock::Now();
            exp_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(n));
        }
        // 3) ModifyHp：+1 / -1 交替，停留在上限附近，不触发死亡路径
        {
            const SteadyNs t0 = MonotonicClock::Now();
            for (std::size_t i = 0; i < n; ++i) {
                const std::int64_t delta = ((i & 1) == 0) ? -1 : 1;
                (void)sys.ModifyHp(ids[i], delta, core::NewTraceID());
            }
            const SteadyNs t1 = MonotonicClock::Now();
            hp_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(n));
        }
        // 4) Save：只入队，不等待落盘（§11）
        {
            const SteadyNs t0 = MonotonicClock::Now();
            for (std::size_t i = 0; i < n; ++i) (void)sys.Save(ids[i]);
            const SteadyNs t1 = MonotonicClock::Now();
            save_samples.push_back(static_cast<double>(t1 - t0) / static_cast<double>(n));
            if (sink.PendingCount() >= 4096) sink.Drain();  // 模拟后台线程消费
        }
    }
    sink.Drain();

    const double recompute_ns = Percentile(rc_samples, 0.5);
    const double add_exp_ns = Percentile(exp_samples, 0.5);
    const double modify_hp_ns = Percentile(hp_samples, 0.5);
    const double save_enqueue_ns = Percentile(save_samples, 0.5);

    // ---- 写机器可读结果 ----
    std::FILE* fp = std::fopen(out_path, "w");
    if (fp == nullptr) {
        ErrorFmt("FAIL: cannot open %s\n", out_path);
        std::exit(1);
    }
    std::fprintf(fp,
                 "characters=%zu\n"
                 "recompute_ns=%.3f\n"
                 "add_exp_ns=%.3f\n"
                 "modify_hp_ns=%.3f\n"
                 "mem_bytes_per_character=%.3f\n"
                 "save_enqueue_ns=%.3f\n",
                 n, recompute_ns, add_exp_ns, modify_hp_ns, mem_per_char, save_enqueue_ns);
    std::fclose(fp);

    LineFmt("characters=%zu recompute_ns=%.3f add_exp_ns=%.3f modify_hp_ns=%.3f "
            "mem_bytes_per_character=%.3f save_enqueue_ns=%.3f\n",
            n, recompute_ns, add_exp_ns, modify_hp_ns, mem_per_char, save_enqueue_ns);
    LineFmt("  (p99: recompute=%.1f add_exp=%.1f modify_hp=%.1f save=%.1f)\n",
            Percentile(rc_samples, 0.99), Percentile(exp_samples, 0.99),
            Percentile(hp_samples, 0.99), Percentile(save_samples, 0.99));
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1000;
    std::size_t iters = 200000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--characters" && i + 1 < argc) {
            n = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else if (a == "--iters" && i + 1 < argc) {
            iters = static_cast<std::size_t>(std::atoll(argv[++i]));
        }
    }

    Line("== TASK-016 role_bench ==\n");
    Run(n, iters, "bench/role.txt");
    return 0;
}
