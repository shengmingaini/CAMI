// server/gamenode/aoi/benchmark/aoi_bench.cpp — TASK-014 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/aoi_<entities>.txt（验收脚本 assert_metric 解析）：
//   entities / query_ns_avg / query_ns_p99 / broadcast_ns_per_target / avg_visible /
//   mem_bytes_per_entity / cross_cell_per_tick
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "test_print.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/aoi/aoi.h"
#include "mmo/game/aoi/dynamic_grid_aoi.h"

namespace {

using namespace mmo::game::aoi;
using mmo::core::SteadyNs;
using mmo::core::MonotonicClock;

constexpr float kViewR = 50.0f;
constexpr float kCell = 20.0f;

// 世界边长：随实体数缩放，使 avg_visible 落在 §22 期望区间（20~60）且格子数可控（内存预算）。
float WorldSide(std::size_t n) {
    return std::max(140.0f, std::sqrt(static_cast<float>(n)) * 12.0f);
}

void RunTier(std::size_t n, std::size_t ticks, const char* out_path) {
    std::mt19937 rng(0xABCDEF01u + static_cast<unsigned>(n));
    const float side = WorldSide(n);
    const float half = side / 2.0f;
    std::uniform_real_distribution<float> d(-half, half);
    std::uniform_real_distribution<float> dz(-5.0f, 5.0f);

    ResetAoiHeapTracking();
    auto aoi = CreateDynamicGridAoi(AoiConfig{kCell, kViewR, n + 16, true});

    std::vector<std::pair<EntityId, Position>> ents;
    ents.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Position p{d(rng), d(rng), dz(rng), 0.0f};
        ents.emplace_back(static_cast<EntityId>(i + 1), p);
        (void)aoi->Enter(static_cast<EntityId>(i + 1), p);
    }

    // ---- QueryVisible 延迟（采样 2000 次随机查询）----
    std::vector<double> q_samples;
    q_samples.reserve(2000);
    std::vector<EntityId> tmp;
    for (int i = 0; i < 2000; ++i) {
        const EntityId id = static_cast<EntityId>((rng() % n) + 1);
        const SteadyNs t0 = MonotonicClock::Now();
        (void)aoi->QueryVisible(id, tmp);
        const SteadyNs t1 = MonotonicClock::Now();
        q_samples.push_back(static_cast<double>(t1 - t0));
    }
    std::sort(q_samples.begin(), q_samples.end());
    double q_avg = 0;
    for (double s : q_samples) q_avg += s;
    q_avg /= static_cast<double>(q_samples.size());
    const double q_p99 = q_samples[static_cast<std::size_t>(q_samples.size() * 0.99)];

    // ---- Broadcast 成本（采样 500 次，复用缓冲区，统计每目标耗时）----
    double bc_total_ns = 0;
    std::size_t bc_total_targets = 0;
    const std::uint8_t pl[] = {1, 2, 3, 4, 5, 6, 7, 8};
    for (int i = 0; i < 500; ++i) {
        const EntityId id = static_cast<EntityId>((rng() % n) + 1);
        const SteadyNs t0 = MonotonicClock::Now();
        auto r = aoi->Broadcast(id, std::span<const std::uint8_t>(pl, 8));
        const SteadyNs t1 = MonotonicClock::Now();
        bc_total_ns += static_cast<double>(t1 - t0);
        bc_total_targets += r.Value();
    }
    const double bc_per_target = bc_total_targets > 0 ? bc_total_ns / static_cast<double>(bc_total_targets)
                                                       : 0.0;

    // ---- 跨格率（随机游走 ticks 步，统计改变格子的比例）----
    std::uniform_real_distribution<float> step(-12.0f, 12.0f);
    std::size_t cross = 0, moves = 0;
    for (std::size_t t = 0; t < ticks; ++t) {
        const EntityId id = static_cast<EntityId>((rng() % n) + 1);
        Position& p = ents[id - 1].second;
        Position to{std::clamp(p.x + step(rng), -half, half),
                    std::clamp(p.y + step(rng), -half, half), p.z, 0.0f};
        auto mv = aoi->Move(id, to);
        if (mv.HasValue()) {
            ++moves;
            if (mv.Value().touched_cells > 0) ++cross;  // touched_cells>0 表示发生了格子扫描
            p = to;
        }
    }
    const double cross_rate = moves > 0 ? static_cast<double>(cross) / static_cast<double>(moves) : 0.0;

    const AoiStats st = aoi->Stats();
    const double mem_per_entity = AoiHeapBytes() / static_cast<double>(n);

    // ---- 写机器可读结果 ----
    std::FILE* fp = std::fopen(out_path, "w");
    if (!fp) {
        mmo::core::test::ErrorFmt("FAIL: cannot open %s\n", out_path);
        std::exit(1);
    }
    std::fprintf(fp,
                 "entities=%zu\n"
                 "query_ns_avg=%.3f\n"
                 "query_ns_p99=%.3f\n"
                 "broadcast_ns_per_target=%.3f\n"
                 "avg_visible=%zu\n"
                 "mem_bytes_per_entity=%.3f\n"
                 "cross_cell_per_tick=%.6f\n",
                 n, q_avg, q_p99, bc_per_target, st.avg_visible, mem_per_entity, cross_rate);
    std::fclose(fp);

    mmo::core::test::LineFmt("entities=%zu query_ns_avg=%.3f query_ns_p99=%.3f "
                             "broadcast_ns_per_target=%.3f avg_visible=%zu "
                             "mem_bytes_per_entity=%.3f cross_cell_per_tick=%.6f\n",
                             n, q_avg, q_p99, bc_per_target, st.avg_visible, mem_per_entity,
                             cross_rate);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::size_t> tiers{100, 500, 1000, 2000};
    std::size_t ticks = 10000;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--entities" && i + 1 < argc) {
            tiers.clear();
            std::string s = argv[++i];
            std::size_t start = 0;
            while (true) {
                std::size_t comma = s.find(',', start);
                std::string tok = (comma == std::string::npos) ? s.substr(start)
                                                              : s.substr(start, comma - start);
                if (!tok.empty()) tiers.push_back(static_cast<std::size_t>(std::atoll(tok.c_str())));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (a == "--ticks" && i + 1 < argc) {
            ticks = static_cast<std::size_t>(std::atoll(argv[++i]));
        }
    }

    mmo::core::test::Line("== TASK-014 aoi_bench ==\n");
    for (std::size_t e : tiers) {
        char path[128];
        std::snprintf(path, sizeof(path), "bench/aoi_%zu.txt", e);
        RunTier(e, ticks, path);
    }
    return 0;
}
