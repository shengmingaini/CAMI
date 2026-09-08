#pragma once

/// TASK-025 · Combat Benchmark 公开接口（§7，冻结契约）。
///
/// Benchmark 是**纯观测者**（§4）：不拥有任何服务状态，被测 Scene 的 Owner 不变；
/// 只允许读与统计，禁止在测量过程中改动仿真逻辑或调整参数。所有数字原样落盘，
/// 禁止后处理 / 四舍五入美化 / 删除不达标的组（§21 Forbidden）。
///
/// 实现见 src/combat_benchmark.cpp（pimpl：公开头不泄漏任何游戏系统头）。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "metrics.h"
#include "scenario.h"
#include "system_probe.h"

#include "mmo/core/error/result.h"

namespace mmo::bench {

class CombatBenchmark {
public:
    CombatBenchmark();
    ~CombatBenchmark();

    CombatBenchmark(const CombatBenchmark&) = delete;
    CombatBenchmark& operator=(const CombatBenchmark&) = delete;
    CombatBenchmark(CombatBenchmark&&) = delete;
    CombatBenchmark& operator=(CombatBenchmark&&) = delete;

    /// 跑单个场景：构造世界 → 预热 warmup_seconds（数据丢弃）→ 计时 duration_seconds。
    /// 按 20Hz 真实节拍推进（每 Tick 工作量独立计时，节拍空档 sleep，不计入样本）。
    core::Result<ScenarioResult> Run(const ScenarioConfig& cfg);

    /// 跑完整矩阵：4 规模 × 5 场景 = 20 组（§8 禁止抽样，必须全跑）。
    /// 每组写 <output_dir>/combat_<players>_<slug>.txt（key=value，供 assert_metric 断言），
    /// 汇总写 <output_dir>/combat_matrix.json。
    core::Result<void> RunMatrix(std::string_view output_dir);

    /// 单场景结果导出 JSON。
    core::Result<void> ExportJson(const ScenarioResult& r, std::string_view path) const;

    /// 全矩阵 Markdown 表格（含八阶段 P95 分解），供 docs/benchmark/combat-report.md 引用。
    core::Result<void> ExportMarkdown(std::string_view path) const;

    const std::vector<ScenarioResult>& Results() const noexcept { return results_; }

    void SetSeed(std::uint32_t seed) noexcept { seed_ = seed; }
    std::uint32_t Seed() const noexcept { return seed_; }

    /// 覆盖矩阵默认时长（秒）：duration = 计时时长，warmup = 预热时长（数据丢弃）。
    void SetDurations(std::uint32_t duration_seconds, std::uint32_t warmup_seconds) noexcept {
        duration_seconds_ = duration_seconds;
        warmup_seconds_ = warmup_seconds;
    }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<ScenarioResult> results_;
    std::uint32_t seed_{42};
    std::uint32_t duration_seconds_{60};
    std::uint32_t warmup_seconds_{5};
};

}  // namespace mmo::bench
