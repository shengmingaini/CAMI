#include "game/content/content_registry.h"

#include <utility>  // std::move

namespace cami {
namespace game {
namespace content {

namespace {

// 内置模块表: 21 机制层 proto → Tier 映射 (docs/game-design/content-modularization.md §6)。
// Tier 0 核心常驻 (启动加载), Tier 1 等级解锁, Tier 2 阶段解锁 (双闸门)。
// 静态构造, 零运行时开销。
const std::vector<ContentModule>& BuiltinModules() {
    static const std::vector<ContentModule> kModules = {
        // ---- Tier 0: 核心常驻 (启动即加载) ----
        {"core_attributes", 0, {}, {"config_stats"}, "",                      0, Phase::kAlpha, 0},
        {"items",          0, {}, {"config_items"},  "",                      0, Phase::kAlpha, 0},
        {"currency",       0, {}, {"config_currencies", "config_balance"}, "", 0, Phase::kAlpha, 0},
        {"inventory",      0, {"items"}, {},                                  "", 0, Phase::kAlpha, 1024},
        {"base_combat",    0, {"core_attributes"}, {"config_balance"},        "", 0, Phase::kAlpha, 2048},
        {"quest_engine",   0, {}, {"config_quests"},                          "", 0, Phase::kAlpha, 512},
        {"world_zones",    0, {}, {"config_zones", "config_creatures", "config_spawns"}, "", 0, Phase::kAlpha, 4096},

        // ---- Tier 1: 玩家等级解锁 (懒加载) ----
        {"skills",      1, {"base_combat"}, {"config_skills"},        "systems/skills/init.lua", 2,  Phase::kAlpha, 1024},
        {"talents",     1, {"skills"},      {"config_sets", "config_buffs"}, "systems/talents/init.lua", 10, Phase::kAlpha, 512},
        {"progression", 1, {"core_attributes"}, {"config_progression"}, "systems/progression/init.lua", 1, Phase::kAlpha, 0},

        // ---- Tier 2: 进阶玩法 (服务器阶段 + 等级双闸门) ----
        {"gems",        2, {"items"},       {"config_gems"},          "systems/gems/init.lua",     20, Phase::kBeta,  1024},
        {"enchanting",  2, {"items"},       {"config_enchantments"},  "systems/enchanting/init.lua", 20, Phase::kBeta, 512},
        {"professions", 2, {"items"},       {"config_professions", "config_vendors"}, "systems/professions/init.lua", 10, Phase::kGamma, 512},
        {"pvp",         2, {"base_combat"}, {"config_pvp"},           "systems/pvp/init.lua",     30, Phase::kGamma,  0},
        {"reputation",  2, {"quest_engine"}, {"config_reputation"},   "systems/reputation/init.lua", 10, Phase::kGamma, 0},
        {"world_events",2, {"quest_engine"}, {"config_world_events"}, "systems/world_events/init.lua", 40, Phase::kRelease, 256},
        {"loot",        2, {"world_zones"}, {"config_loot"},          "systems/loot/init.lua",    10, Phase::kBeta,  2048},
    };
    return kModules;
}

}  // namespace

ContentRegistry& ContentRegistry::Instance() {
    static ContentRegistry inst;
    return inst;
}

ContentRegistry::ContentRegistry() {
    // 内置表预置 (实例首次构造时一次性)
    for (const auto& m : BuiltinModules()) {
        modules_[m.id] = m;
        states_[m.id] = ModuleState::kInactive;
    }
}

const ContentModule* ContentRegistry::Find(const std::string& id) const {
    auto it = modules_.find(id);
    return it == modules_.end() ? nullptr : &it->second;
}

void ContentRegistry::Register(const ContentModule& m) {
    modules_[m.id] = m;
    auto it = states_.find(m.id);
    if (it == states_.end() || it->second == ModuleState::kInactive) {
        states_[m.id] = ModuleState::kInactive;
    }
}

ModuleState ContentRegistry::State(const std::string& id) const {
    auto it = states_.find(id);
    return it == states_.end() ? ModuleState::kInactive : it->second;
}

std::size_t ContentRegistry::ActiveCount() const {
    std::size_t n = 0;
    for (const auto& kv : states_) {
        if (kv.second == ModuleState::kActive) ++n;
    }
    return n;
}

bool ContentRegistry::EnsureLoaded(const std::string& id, std::string* err) {
    std::vector<std::string> visiting;
    if (!ActivateRecursive(id, err, visiting)) {
        return false;
    }
    return true;
}

bool ContentRegistry::ActivateRecursive(const std::string& id, std::string* err,
                                        std::vector<std::string>& visiting) {
    // 循环依赖检测
    for (const auto& v : visiting) {
        if (v == id) {
            if (err) *err = "circular dependency involving '" + id + "'";
            return false;
        }
    }
    const ContentModule* mod = Find(id);
    if (!mod) {
        if (err) *err = "module '" + id + "' not registered";
        return false;
    }
    ModuleState st = State(id);
    if (st == ModuleState::kActive) return true;        // 已激活: 幂等
    if (st == ModuleState::kLoading) {                  // 理论上不可达 (visiting 已拦)
        if (err) *err = "module '" + id + "' already loading";
        return false;
    }

    states_[id] = ModuleState::kLoading;
    visiting.push_back(id);

    // 依赖拓扑: 先激活依赖
    for (const auto& dep : mod->deps) {
        if (!ActivateRecursive(dep, err, visiting)) {
            states_[id] = ModuleState::kInactive;
            visiting.pop_back();
            return false;
        }
    }
    visiting.pop_back();

    // 激活钩子: 配置加载 / Lua require / EventBus 接线由上层注入
    if (hook_) {
        hook_(*mod);
    }
    states_[id] = ModuleState::kActive;
    return true;
}

bool ContentRegistry::IsUnlocked(const std::string& id, int player_level,
                                 Phase server_phase) const {
    const ContentModule* mod = Find(id);
    if (!mod) return false;
    // 内容闸门: 服务器阶段 >= 模块最低阶段
    if (static_cast<int>(server_phase) < static_cast<int>(mod->min_phase)) return false;
    // 个人闸门: 玩家等级 >= 模块等级门槛 (且不超过当前阶段上限)
    if (player_level < mod->level_gate) return false;
    if (player_level > LevelCap(server_phase)) return false;
    return true;
}

int ContentRegistry::LevelCap(Phase p) const {
    const int idx = static_cast<int>(p);
    return (idx >= 0 && idx < 4) ? phase_caps_[idx] : 0;
}

void ContentRegistry::SetLevelCap(Phase p, int cap) {
    const int idx = static_cast<int>(p);
    if (idx >= 0 && idx < 4 && cap > 0) {
        phase_caps_[idx] = cap;
    }
}

void ContentRegistry::SetActivateHook(ActivateHook h) { hook_ = std::move(h); }
void ContentRegistry::ClearActivateHook()             { hook_ = nullptr; }

}  // namespace content
}  // namespace game
}  // namespace cami
