// server/gamenode/entity/tests/entity_bench.cpp — TASK-011 §18 Benchmark
//
// 指标（机器可读 key=value，供验收脚本 assert_metric 解析）：
//   create_ns_per_entity=   单实体创建耗时（ns），§22 < 100ns
//   destroy_ns_per_entity=  单实体销毁+Flush 耗时（ns），§22 < 80ns
//   find_ns=                单次 Find 耗时（ns），§22 < 20ns
//   iterate_ns_per_1k=      遍历 1000 个实体的耗时（ns），§22 < 5us
//   mem_bytes_per_entity=   单实体内存（字节，不含组件），§22 < 256B
//
// 用法：entity_bench [--entities N]   默认 100000
// 输出：bench/entity.txt

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_manager.h"

namespace {

using namespace mmo::game;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
namespace core = mmo::core;

// 基准组件：用于遍历吞吐测量（dense 连续）。
class BenchTag : public Component<BenchTag> {
public:
    std::uint64_t v = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::size_t entities = 100'000;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--entities") {
            entities = static_cast<std::size_t>(std::stoull(argv[i + 1]));
        }
    }

    core::EventBus bus;

    // ---- 1. 创建耗时（§22 < 100ns）----
    {
        EntityManager mgr(&bus);
        const auto t0 = core::MonotonicClock::Now();
        std::vector<EntityId> ids;
        ids.reserve(entities);
        for (std::size_t i = 0; i < entities; ++i) {
            auto r = mgr.Create(EntityType::Player, 1, Position{});
            if (!r.HasValue()) {
                ErrorFmt("entity_bench: create failed at %zu\n", i);
                return 1;
            }
            ids.push_back(r.Value()->Id());
        }
        const double create_ns = static_cast<double>(core::MonotonicClock::Now() - t0) /
                                 static_cast<double>(entities);

        // ---- 2. 查找耗时（§22 < 20ns）----
        const auto tf = core::MonotonicClock::Now();
        std::size_t found = 0;
        for (std::size_t i = 0; i < entities; ++i) {
            if (mgr.Find(ids[i]) != nullptr) {
                ++found;
            }
        }
        const double find_ns = static_cast<double>(core::MonotonicClock::Now() - tf) /
                               static_cast<double>(entities);

        // ---- 3. 销毁 + Flush 耗时（§22 < 80ns）----
        const auto td = core::MonotonicClock::Now();
        for (std::size_t i = 0; i < entities; ++i) {
            (void)mgr.Destroy(ids[i]);
        }
        mgr.FlushDeferred();
        const double destroy_ns = static_cast<double>(core::MonotonicClock::Now() - td) /
                                  static_cast<double>(entities);

        (void)found;

        // ---- 4. 遍历 1000 实体耗时（§22 < 5us）----
        EntityManager mgr2(&bus);
        constexpr std::size_t kIterN = 1000;
        std::vector<EntityId> iter_ids;
        iter_ids.reserve(kIterN);
        for (std::size_t i = 0; i < kIterN; ++i) {
            auto r = mgr2.Create(EntityType::Item, 1, Position{});
            r.Value()->AddComponent<BenchTag>()->v = i;
            iter_ids.push_back(r.Value()->Id());
        }
        constexpr std::size_t kRounds = 200;
        volatile std::uint64_t sink = 0;
        const auto ti = core::MonotonicClock::Now();
        for (std::size_t round = 0; round < kRounds; ++round) {
            mgr2.Each<BenchTag>([&](Entity&, BenchTag& c) { sink += c.v; });
        }
        const double iter_total = static_cast<double>(core::MonotonicClock::Now() - ti);
        const double iterate_ns_per_1k = iter_total / static_cast<double>(kRounds);

        const std::size_t mem = EntityManager::MemoryBytesPerEntity();

        Line("== TASK-011 entity_bench ==\n");
        LineFmt("entities=%zu\n", entities);
        LineFmt("create_ns_per_entity=%.3f\n", create_ns);
        LineFmt("find_ns=%.3f\n", find_ns);
        LineFmt("destroy_ns_per_entity=%.3f\n", destroy_ns);
        LineFmt("iterate_ns_per_1k=%.3f\n", iterate_ns_per_1k);
        LineFmt("mem_bytes_per_entity=%zu\n", mem);

        std::ofstream ofs("bench/entity.txt");
        if (!ofs) {
            ErrorFmt("entity_bench: cannot write bench/entity.txt\n");
            return 1;
        }
        ofs << "entities=" << entities << "\n";
        ofs << "create_ns_per_entity=" << create_ns << "\n";
        ofs << "find_ns=" << find_ns << "\n";
        ofs << "destroy_ns_per_entity=" << destroy_ns << "\n";
        ofs << "iterate_ns_per_1k=" << iterate_ns_per_1k << "\n";
        ofs << "mem_bytes_per_entity=" << mem << "\n";
        ofs.close();

        LineFmt("entity_bench done -> bench/entity.txt\n");
    }
    return 0;
}
