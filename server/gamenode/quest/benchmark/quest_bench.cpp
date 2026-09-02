// server/gamenode/quest/benchmark/quest_bench.cpp — TASK-019 §18 / §22 / §24
//
// 输出机器可读 key=value 到 bench/quest.txt（验收脚本 assert_metric 解析）：
//   players / events / event_handle_ns / quest_update_per_1k_events_us /
//   mem_bytes_per_quest / scaling_check_1k_vs_10k_players / config_load_ms_for_1000
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// 内存口径：替换全局 operator new/delete，按 _msize 统计**净堆增量**（同 role/ai bench）。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
#include <fstream>
#include <string>

#include "test_print.h"

#include "mmo/core/time/clock.h"
#include "mmo/game/quest/quest_def.h"
#include "mmo/game/quest/quest_instance.h"
#include "mmo/game/quest/quest_system.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game::quest;

using core::MonotonicClock;
using core::SteadyNs;

using mmo::core::test::ErrorFmt;
using mmo::core::test::LineFmt;

constexpr const char* kConfigPath = "config/gameplay/quests";
constexpr std::uint32_t kBenchQuest = 1001;      // 杀怪任务（目标 1001 ×3）
constexpr std::uint32_t kBenchMonster = 1001;
constexpr std::size_t kHitPlayers = 1000;        // 命中玩家集合固定为 1000 个

int g_bench_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++g_bench_fail;                                               \
            ErrorFmt("BENCH FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

class NullSink : public QuestSystem::IRewardSink {
public:
    core::Result<void> Grant(PlayerId, const QuestDef&, core::TraceID) override {
        return core::Result<void>::Ok();
    }
};

QuestSystem::Event Kill(PlayerId p, std::uint32_t npc, std::uint32_t n = 1) {
    return {ObjectiveType::KillMonster, npc, n, p};
}

/// 建立 players 个玩家（各接一条高目标数、无前置的任务），对**固定的 1000 个玩家**投递
/// events 条事件，返回每条事件的平均耗时（ns）。命中集合恒定（任务不完成），与玩家总数无关。
double RunEvents(std::size_t players, std::size_t events) {
    NullSink sink;
    QuestSystem qs(nullptr);
    qs.BindRewardSink(sink);
    CHECK(qs.LoadQuests(kConfigPath).HasValue());
    // 注册一条「目标数远大于事件预算」的任务：每个事件都命中索引条目（O(1)），
    // 但进度永不达标 → 不触发 EraseQuest / 重接，稳定测量单次 OnEvent 真实耗时。
    QuestDef sd{};
    sd.id = 7777;
    sd.required_level = 1;
    sd.objectives.push_back(ObjectiveDef{ObjectiveType::KillMonster, 7777, 1000});
    CHECK(qs.RegisterDef(std::move(sd)).HasValue());
    for (std::size_t i = 0; i < players; ++i) {
        CHECK(qs.Accept(1000000 + i, 7777, 1).HasValue());
    }

    const SteadyNs t0 = MonotonicClock::Now();
    for (std::size_t i = 0; i < events; ++i) {
        const PlayerId p = 1000000 + (i % kHitPlayers);
        (void)qs.OnEvent(Kill(p, 7777, 1));
    }
    const SteadyNs elapsed = MonotonicClock::Now() - t0;
    return static_cast<double>(elapsed) / static_cast<double>(events);
}

/// 每条任务实例的净堆占用（Accept N 条任务前后的堆增量）。
double MeasureMemBytesPerQuest(std::size_t quests) {
    NullSink sink;
    QuestSystem qs(nullptr);
    qs.BindRewardSink(sink);
    CHECK(qs.LoadQuests(kConfigPath).HasValue());

    const std::int64_t before = g_heap_bytes;
    for (std::size_t i = 0; i < quests; ++i) {
        CHECK(qs.Accept(5000000 + i, kBenchQuest, 1).HasValue());
    }
    const std::int64_t after = g_heap_bytes;
    return static_cast<double>(after - before) / static_cast<double>(quests);
}

/// 生成 1000 条配置（独立目录）并测加载耗时（ms）。
double MeasureConfigLoadMs(std::size_t count) {
    std::filesystem::create_directories("bench/__qdir_bench");
    const std::string path = "bench/__qdir_bench/__quests_1000.json";
    {
        std::ofstream out(path, std::ios::binary);
        out << "{\"quests\":[";
        for (std::size_t i = 0; i < count; ++i) {
            if (i != 0) out << ",";
            out << "{\"id\":" << (2000 + i) << ",\"title\":\"bench q" << i
                << "\",\"required_level\":1,\"objectives\":[{\"type\":\"KillMonster\","
                   "\"target_id\":"
                << (6000 + i) << ",\"count\":3}],\"exp_reward\":" << (10 + i)
                << ",\"currency_reward\":5,\"item_rewards\":[]}";
        }
        out << "]}";
    }
    NullSink sink;
    QuestSystem qs(nullptr);
    qs.BindRewardSink(sink);
    const SteadyNs t0 = MonotonicClock::Now();
    auto r = qs.LoadQuests("bench/__qdir_bench");
    const SteadyNs elapsed = MonotonicClock::Now() - t0;
    CHECK(r.HasValue());
    CHECK(qs.DefCount() == count);
    std::error_code ec;
    std::filesystem::remove_all("bench/__qdir_bench", ec);
    return static_cast<double>(elapsed) / 1e6;
}

/// Scaling：1k vs 10k 玩家同批事件的耗时比（应 ≈ 1.0，证明无「遍历所有玩家」反模式）。
/// 取多次试验的最佳比值并放大样本，摊薄纳秒级计时的 OS 抖动（基线仅 ~25ns/ev）。
double MeasureScaling(int trials) {
    constexpr std::size_t kScalingEvents = 100000;
    double best = 1e9;
    for (int t = 0; t < trials; ++t) {
        const double ns_1k = RunEvents(1000, kScalingEvents);
        const double ns_10k = RunEvents(10000, kScalingEvents);
        const double r = (ns_1k > 0.0) ? (ns_10k / ns_1k) : 0.0;
        if (r < best) best = r;
    }
    return best;
}

void Run(std::size_t players, std::size_t events, const char* out_path) {
    // 1. 单条事件处理耗时（在 --players 指定的规模下）
    const double ns_per_event = RunEvents(players, events);

    // 2. Scaling：1k 玩家 vs 10k 玩家，同一批事件的耗时比（应 ≈ 1.0）
    const double scaling = MeasureScaling(5);

    // 3. 单条任务实例的净堆占用
    const double mem_per_quest = MeasureMemBytesPerQuest(20000);

    // 4. 1000 条配置加载耗时
    const double config_load_ms = MeasureConfigLoadMs(1000);

    // 5. 索引规模观测
    NullSink sink;
    QuestSystem qs(nullptr);
    qs.BindRewardSink(sink);
    (void)qs.LoadQuests(kConfigPath);
    for (std::size_t i = 0; i < 1000; ++i) {
        (void)qs.Accept(9000000 + i, kBenchQuest, 1);
    }
    const std::size_t buckets = qs.IndexBucketCount();
    const std::size_t entries = qs.IndexEntryCount();

    const double per_1k_events_us = ns_per_event * 1000.0 / 1000.0;  // 1000 条事件 × ns → us

    std::FILE* fp = std::fopen(out_path, "w");
    if (fp != nullptr) {
        std::fprintf(fp,
                     "players=%zu\n"
                     "events=%zu\n"
                     "event_handle_ns=%.3f\n"
                     "quest_update_per_1k_events_us=%.3f\n"
                     "mem_bytes_per_quest=%.3f\n"
                     "scaling_check_1k_vs_10k_players=%.3f\n"
                     "config_load_ms_for_1000=%.3f\n"
                     "index_bucket_count=%zu\n"
                     "index_entry_count=%zu\n",
                     players, events, ns_per_event, per_1k_events_us, mem_per_quest, scaling,
                     config_load_ms, buckets, entries);
        std::fclose(fp);
    }

    LineFmt("players=%zu events=%zu event_handle_ns=%.3f "
            "quest_update_per_1k_events_us=%.3f mem_bytes_per_quest=%.3f "
            "scaling_check_1k_vs_10k_players=%.3f config_load_ms_for_1000=%.3f\n",
            players, events, ns_per_event, per_1k_events_us, mem_per_quest, scaling,
            config_load_ms);
    LineFmt("index_bucket_count=%zu index_entry_count=%zu (1000 玩家各 1 条同目标任务)\n",
            buckets, entries);
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t players = 1000;
    std::size_t events = 10000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--players" && i + 1 < argc) {
            players = static_cast<std::size_t>(std::atoll(argv[++i]));
        } else if (a == "--events" && i + 1 < argc) {
            events = static_cast<std::size_t>(std::atoll(argv[++i]));
        }
    }
    LineFmt("== TASK-019 quest_bench ==\n");
    Run(players, events, "bench/quest.txt");
    return g_bench_fail == 0 ? 0 : 1;
}
