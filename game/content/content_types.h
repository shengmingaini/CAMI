#pragma once
// ============================================================================
// game/content/content_types.h — 内容模块化基础类型 (纯 STL, 零外部依赖)
// ----------------------------------------------------------------------------
// 玩法内容 (宝石/附魔/天赋/技能/等级上限等) 声明为"内容模块", 按 Tier 分层、
// 双闸门 (服务器 phase + 玩家等级) 解锁、按需懒加载。
// 详见 docs/game-design/content-modularization.md。
// ============================================================================
#include <cstddef>
#include <string>
#include <vector>

namespace cami {
namespace game {
namespace content {

// 服务器内容阶段 (等级上限分阶段, 2026-08-12 决策 30/45/60/70)。
// 后期新增阶段: 用 SetLevelCap(phase, cap) 扩展 (content_registry 预留接口),
// 或直接在本枚举追加值 + content_registry.cpp 的 kPhaseLevelCaps 补一行。
enum class Phase : int {
    kAlpha   = 0,  // 30 级上限
    kBeta    = 1,  // 45 级上限
    kGamma   = 2,  // 60 级上限
    kRelease = 3,  // 70 级上限
};

// 内容模块声明 (静态注册表项, 纯数据, 不含玩法逻辑 — 玩法全走 Lua)。
struct ContentModule {
    std::string              id;            // 模块唯一名, 如 "gems"
    int                      tier = 0;      // Tier 0=核心常驻 / 1=等级解锁 / 2=阶段解锁 / 3=扩展
    std::vector<std::string> deps;          // 依赖模块 (先加载), 如 gems 依赖 items
    std::vector<std::string> config_sets;   // 对应 proto ConfigSet, 如 {"config_gems"}
    std::string              lua_entry;     // Lua 模块入口, 如 "systems/gems/init.lua"
    int                      level_gate = 0;// 玩家等级门槛 (个人闸门)
    Phase                    min_phase = Phase::kAlpha;  // 服务器阶段门槛 (内容闸门)
    std::size_t              pool_entities = 0;          // 对象池预分配规格 (激活时)
};

// 模块运行时状态
enum class ModuleState : int {
    kInactive = 0,  // 未加载 (不占任何内存)
    kLoading  = 1,  // 正在加载 (依赖拓扑处理中)
    kActive   = 2,  // 已激活 (配置/Lua/对象池已就位, 驻留)
};

}  // namespace content
}  // namespace game
}  // namespace cami
