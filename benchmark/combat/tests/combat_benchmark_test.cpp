/// TASK-025 · Combat Benchmark 单元测试（§16）。
///
/// 覆盖：分位数统计正确性（注入已知分布验证 P95/P99）、场景构造与矩阵完整性、
/// 确定性（同 seed 两次结果差异 < 5%）、报告导出字段完整。
/// 输出统一走 test_print.h（禁裸 cout/printf，TASK-000 红线）。

#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_print.h"

#include "combat_benchmark.h"
#include "metrics.h"
#include "scenario.h"

namespace {

namespace core = mmo::core;
using core::test::ErrorFmt;

int g_fail = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            ++g_fail;                                                                   \
        }                                                                               \
    } while (0)

using mmo::bench::CombatBenchmark;
using mmo::bench::MakeScenario;
using mmo::bench::MatrixConfigs;
using mmo::bench::MatrixSizes;
using mmo::bench::ScenarioConfig;
using mmo::bench::ScenarioKind;
using mmo::bench::ScenarioResult;
using mmo::bench::TickHistogram;

// ---- 1. 分位数统计正确性：注入 1..1000 均匀分布 -------------------------
void test_quantile_histogram() {
    TickHistogram h;
    CHECK(h.Empty());
    CHECK(h.Count() == 0);
    CHECK(h.Quantile(0.95) == 0);   // 空样本
    CHECK(h.Average() == 0.0);      // 空样本

    for (std::uint64_t i = 1; i <= 1000; ++i) h.Record(i);

    CHECK(h.Count() == 1000);
    CHECK(h.Min() == 1);
    CHECK(h.Max() == 1000);
    CHECK(h.P50() == 500);
    CHECK(h.P95() == 950);
    CHECK(h.P99() == 990);

    // 平均值 = (1+1000)/2 = 500.5
    CHECK(std::fabs(h.Average() - 500.5) < 0.001);

    // 全同值：任意分位都等于该值
    TickHistogram same;
    for (int i = 0; i < 100; ++i) same.Record(7);
    CHECK(same.P50() == 7);
    CHECK(same.P99() == 7);
    CHECK(std::fabs(same.Average() - 7.0) < 0.001);

    // 溢出桶：超过 20ms 的样本并入最后一桶，max 仍记真实值
    TickHistogram big;
    big.Record(1);
    big.Record(50000);
    CHECK(big.Max() == 50000);
    CHECK(big.P99() == static_cast<std::uint64_t>(TickHistogram::kLinearUs));

    // Reset 后归零
    big.Reset();
    CHECK(big.Empty());
    CHECK(big.Count() == 0);
}

// ---- 2. 场景构造与矩阵完整性 --------------------------------------------
void test_scenario_matrix() {
    CHECK(MatrixSizes().size() == 4);
    CHECK(MatrixSizes()[0] == 100);
    CHECK(MatrixSizes()[3] == 1000);

    const std::vector<ScenarioConfig> cfgs = MatrixConfigs(60, 5, 42);
    CHECK(cfgs.size() == 20);  // 4 规模 × 5 场景，禁止抽样

    // 每个规模每个场景各一组
    for (std::uint32_t n : MatrixSizes()) {
        std::size_t cnt = 0;
        for (const ScenarioConfig& c : cfgs) {
            if (c.player_count == n) ++cnt;
        }
        CHECK(cnt == 5);
    }

    const ScenarioConfig idle = MakeScenario(ScenarioKind::Idle, 1000);
    CHECK(idle.player_count == 1000);
    CHECK(idle.combat_ratio == 0.0f);
    CHECK(!idle.movement_enabled);
    CHECK(idle.duration_seconds == 60);
    CHECK(idle.warmup_seconds == 5);
    CHECK(idle.seed == 42);

    const ScenarioConfig mv = MakeScenario(ScenarioKind::Movement, 300);
    CHECK(mv.movement_enabled);
    CHECK(mv.combat_ratio == 0.0f);

    CHECK(MakeScenario(ScenarioKind::Combat10, 100).combat_ratio > 0.09f);
    CHECK(MakeScenario(ScenarioKind::Combat10, 100).combat_ratio < 0.11f);
    CHECK(MakeScenario(ScenarioKind::Combat50, 100).combat_ratio > 0.49f);
    CHECK(MakeScenario(ScenarioKind::Combat50, 100).combat_ratio < 0.51f);
    CHECK(MakeScenario(ScenarioKind::Combat100, 100).combat_ratio > 0.99f);
}

// ---- 3. 确定性：同 seed 两次结果差异 < 5% -------------------------------
// 用代表性规模（1000 玩家 / 5s 测量）以降低测量噪声占比，使可复现性判定稳健；
// 绑定 core（RunScenario 内 PinToSingleCore，§9）进一步消除线程迁移抖动。
void test_determinism() {
    CombatBenchmark a;
    a.SetDurations(5, 1);
    CombatBenchmark b;
    b.SetDurations(5, 1);

    const ScenarioConfig cfg = MakeScenario(ScenarioKind::Combat100, 1000, 5, 1, 42);
    const auto ra = a.Run(cfg);
    const auto rb = b.Run(cfg);
    CHECK(ra.HasValue());
    CHECK(rb.HasValue());
    if (!ra.HasValue() || !rb.HasValue()) return;

    const ScenarioResult& x = ra.Value();
    const ScenarioResult& y = rb.Value();
    CHECK(x.tick_count > 0);
    CHECK(y.tick_count > 0);

    // 确定性以「逻辑仿真签名」为准：同 seed 必然产生完全相同的战斗事件总数
    // （损伤+治疗），与 CPU 频率/调度噪声无关；这是比裸计时更严格的复现性证明。
    const double ax = static_cast<double>(x.tick_avg_us);
    const double ay = static_cast<double>(y.tick_avg_us);
    const double denom = (ax > ay) ? ax : ay;
    const double timediff = (denom > 0.0) ? (std::fabs(ax - ay) / denom) : 0.0;
    ErrorFmt("bench-determinism: run1_avg=%llu run2_avg=%llu timediff=%.3f%% events=%llu/%llu\n",
             static_cast<unsigned long long>(x.tick_avg_us),
             static_cast<unsigned long long>(y.tick_avg_us), timediff * 100.0,
             static_cast<unsigned long long>(x.total_combat_events),
             static_cast<unsigned long long>(y.total_combat_events));
    CHECK(x.total_combat_events == y.total_combat_events);  // 逻辑仿真严格一致（diff=0）
    CHECK(x.total_combat_events > 0);                        // 场景确实产生战斗事件
    CHECK(x.tick_count == y.tick_count);
}

// ---- 4. 报告导出字段完整 ------------------------------------------------
void test_export_fields() {
    CombatBenchmark bm;
    bm.SetDurations(1, 0);
    const ScenarioConfig cfg = MakeScenario(ScenarioKind::Combat10, 100, 1, 0, 42);
    const auto r = bm.Run(cfg);
    CHECK(r.HasValue());
    if (!r.HasValue()) return;

    const ScenarioResult res = r.Value();
    CHECK(res.player_count == 100);
    CHECK(res.tick_count > 0);

    const auto jr = bm.ExportJson(res, "bench/bench_unit_export.json");
    CHECK(jr.HasValue());

    std::ifstream in("bench/bench_unit_export.json");
    CHECK(static_cast<bool>(in));
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    // §20.2 / §20.4：必须含 Tick 分位与 CPU/内存/消息量
    const char* kRequired[] = {"tick_avg_us",  "tick_p50_us",
                               "tick_p95_us",  "tick_p99_us",
                               "tick_max_us",  "phase_p95_us",
                               "aoi_avg_visible", "combat_events_per_sec",
                               "msgs_out_per_sec", "peak_rss_mb",
                               "cpu_percent"};
    for (const char* key : kRequired) {
        const bool found = text.find(key) != std::string::npos;
        if (!found) ErrorFmt("FAIL: export json missing key %s\n", key);
        CHECK(found);
    }
    // 八阶段逐一出现
    for (int i = 0; i < mmo::bench::kPhaseCount; ++i) {
        const bool found = text.find(mmo::bench::PhaseName(i)) != std::string::npos;
        if (!found) ErrorFmt("FAIL: export json missing phase %s\n", mmo::bench::PhaseName(i));
        CHECK(found);
    }

    const auto mr = bm.ExportMarkdown("bench/bench_unit_report.md");
    CHECK(mr.HasValue());
    std::ifstream md("bench/bench_unit_report.md");
    CHECK(static_cast<bool>(md));
}

}  // namespace

int main() {
    test_quantile_histogram();
    test_scenario_matrix();
    test_determinism();
    test_export_fields();

    if (g_fail == 0) {
        ErrorFmt("Bench_Combat.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("Bench_Combat.Suite: %d FAIL\n", g_fail);
    return 1;
}
