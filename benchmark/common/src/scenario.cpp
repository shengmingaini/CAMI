#include "scenario.h"

namespace mmo::bench {

const char* PhaseName(int phase) noexcept {
    switch (phase) {
        case 0: return "Input";
        case 1: return "Movement";
        case 2: return "AOI";
        case 3: return "Combat";
        case 4: return "Buff";
        case 5: return "Quest";
        case 6: return "Event";
        case 7: return "Replication";
        default: return "Unknown";
    }
}

const char* ScenarioName(ScenarioKind kind) noexcept {
    switch (kind) {
        case ScenarioKind::Idle: return "Idle";
        case ScenarioKind::Movement: return "Movement";
        case ScenarioKind::Combat10: return "Combat10";
        case ScenarioKind::Combat50: return "Combat50";
        case ScenarioKind::Combat100: return "Combat100";
        default: return "Unknown";
    }
}

const char* ScenarioSlug(ScenarioKind kind) noexcept {
    switch (kind) {
        case ScenarioKind::Idle: return "idle";
        case ScenarioKind::Movement: return "movement";
        case ScenarioKind::Combat10: return "10pct";
        case ScenarioKind::Combat50: return "50pct";
        case ScenarioKind::Combat100: return "100pct";
        default: return "unknown";
    }
}

const std::vector<std::uint32_t>& MatrixSizes() noexcept {
    static const std::vector<std::uint32_t> kSizes{100, 300, 500, 1000};
    return kSizes;
}

ScenarioConfig MakeScenario(ScenarioKind kind, std::uint32_t players,
                            std::uint32_t duration_seconds, std::uint32_t warmup_seconds,
                            std::uint32_t seed) noexcept {
    ScenarioConfig c;
    c.player_count = players;
    c.duration_seconds = duration_seconds;
    c.warmup_seconds = warmup_seconds;
    c.seed = seed;
    c.kind = kind;
    switch (kind) {
        case ScenarioKind::Idle:
            c.combat_ratio = 0.0f;
            c.movement_enabled = false;
            break;
        case ScenarioKind::Movement:
            c.combat_ratio = 0.0f;
            c.movement_enabled = true;
            break;
        case ScenarioKind::Combat10:
            c.combat_ratio = 0.10f;
            c.movement_enabled = false;
            break;
        case ScenarioKind::Combat50:
            c.combat_ratio = 0.50f;
            c.movement_enabled = false;
            break;
        case ScenarioKind::Combat100:
        default:
            c.combat_ratio = 1.00f;
            c.movement_enabled = false;
            break;
    }
    return c;
}

std::vector<ScenarioConfig> MatrixConfigs(std::uint32_t duration_seconds,
                                          std::uint32_t warmup_seconds, std::uint32_t seed) {
    std::vector<ScenarioConfig> out;
    out.reserve(MatrixSizes().size() * static_cast<std::size_t>(kScenarioCount));
    for (std::uint32_t n : MatrixSizes()) {
        for (int k = 0; k < kScenarioCount; ++k) {
            out.push_back(MakeScenario(static_cast<ScenarioKind>(k), n, duration_seconds,
                                       warmup_seconds, seed));
        }
    }
    return out;
}

}  // namespace mmo::bench
