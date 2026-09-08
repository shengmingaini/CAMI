#pragma once

/// TASK-025 · 场景定义与结果模型（§7 冻结接口 / §8 测试矩阵）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mmo::bench {

/// 八阶段顺序（PROJECT_REQUIREMENTS §1.5，与 TASK-013 kTickPhaseOrder 一致，禁止运行期调整）。
constexpr int kPhaseCount = 8;

enum class Phase : int {
    Input = 0,
    Movement = 1,
    Aoi = 2,
    Combat = 3,
    Buff = 4,
    Quest = 5,
    Event = 6,
    Replication = 7,
};

const char* PhaseName(int phase) noexcept;

/// 五种场景（§8 测试矩阵：Idle / Movement / 10% / 50% / 100% Combat）。
enum class ScenarioKind : int {
    Idle = 0,      // 仅心跳
    Movement = 1,  // 全员随机移动
    Combat10 = 2,  // 10% 实体战斗
    Combat50 = 3,  // 50% 实体战斗
    Combat100 = 4, // 100% 实体战斗
};

constexpr int kScenarioCount = 5;
const char* ScenarioName(ScenarioKind kind) noexcept;
/// 输出文件名片段：idle / movement / 10pct / 50pct / 100pct
const char* ScenarioSlug(ScenarioKind kind) noexcept;

struct ScenarioConfig {
    std::uint32_t player_count{1000};
    float combat_ratio{0.5f};     // 0.0 ~ 1.0
    bool movement_enabled{false};
    std::uint32_t duration_seconds{60};
    std::uint32_t warmup_seconds{5};
    std::uint32_t seed{42};

    ScenarioKind kind{ScenarioKind::Combat50};
};

struct ScenarioResult {
    std::uint32_t player_count{0};
    float combat_ratio{0.0f};

    std::uint64_t tick_avg_us{0};
    std::uint64_t tick_p50_us{0};
    std::uint64_t tick_p95_us{0};
    std::uint64_t tick_p99_us{0};
    std::uint64_t tick_max_us{0};

    std::uint64_t phase_us[kPhaseCount]{};  // 各阶段 P95（µs）

    std::uint64_t aoi_avg_visible{0};
    std::uint64_t combat_events_per_sec{0};
    std::size_t peak_rss_mb{0};
    double cpu_percent{0.0};
    std::uint64_t msgs_out_per_sec{0};

    std::uint64_t tick_count{0};  // 计入统计的 Tick 数（预热后）
    std::uint64_t total_combat_events{0};  // 损伤+治疗事件总数（确定性签名，与 seed 严格对应）
    std::string name;             // 场景名（报告用）
};

/// 按场景类型构造配置。
ScenarioConfig MakeScenario(ScenarioKind kind, std::uint32_t players,
                            std::uint32_t duration_seconds = 60,
                            std::uint32_t warmup_seconds = 5,
                            std::uint32_t seed = 42) noexcept;

/// 完整矩阵：4 规模 × 5 场景 = 20 组（§8 禁止抽样，必须全跑）。
std::vector<ScenarioConfig> MatrixConfigs(std::uint32_t duration_seconds = 60,
                                          std::uint32_t warmup_seconds = 5,
                                          std::uint32_t seed = 42);

/// 规模档位：100 / 300 / 500 / 1000。
const std::vector<std::uint32_t>& MatrixSizes() noexcept;

}  // namespace mmo::bench
