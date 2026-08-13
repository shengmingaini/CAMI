#pragma once
// ============================================================================
// game/content/content_registry.h — 内容模块注册表与懒加载器 (纯 STL)
// ----------------------------------------------------------------------------
// 职责:
//   1. 静态声明全部内容模块 (Tier 0-2, 21 配置映射, 见 content_registry.cpp);
//   2. EnsureLoaded(): 按依赖拓扑懒加载, 幂等, 激活钩子由上层接线;
//   3. IsUnlocked(): 双闸门 (服务器 phase ∧ 玩家等级);
//   4. LevelCap(): 分阶段等级上限 (30/45/60/70), SetLevelCap() 预留后期扩展。
// C++ 侧只做"注册 + 加载调度", 玩法逻辑全走 Lua (红线合规)。
// ============================================================================
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/content/content_types.h"

namespace cami {
namespace game {
namespace content {

// 模块激活钩子: 上层注入 (ConfigManager/LuaModuleLoader/EventBus/对象池接线)。
// 在 EnsureLoaded 中按依赖顺序同步调用。
using ActivateHook = std::function<void(const ContentModule&)>;

class ContentRegistry {
public:
    // 单例 (注册表只读, 无锁热路径)
    static ContentRegistry& Instance();

    // --- 注册表操作 ---
    // 注册/覆盖模块 (内置表已预置, 测试与热更可覆盖)
    void Register(const ContentModule& m);
    // 按 id 查模块 (未注册返回 nullptr)
    const ContentModule* Find(const std::string& id) const;

    // --- 懒加载 ---
    // 激活模块 (含依赖, 拓扑排序, 幂等)。失败返回 false 并写 err。
    bool EnsureLoaded(const std::string& id, std::string* err = nullptr);
    // 模块当前状态
    ModuleState State(const std::string& id) const;
    // 已激活模块数
    std::size_t ActiveCount() const;

    // --- 双闸门 ---
    // 服务器阶段 + 玩家等级均达标才可见 (个人解锁)
    bool IsUnlocked(const std::string& id, int player_level, Phase server_phase) const;

    // --- 分阶段等级上限 ---
    // 当前阶段等级上限 (Alpha=30/Beta=45/Gamma=60/Release=70)
    int LevelCap(Phase p) const;
    // 后期新增阶段: 覆盖某阶段上限 (接口预留, 无需改枚举/内置表)
    void SetLevelCap(Phase p, int cap);

    // --- 接线 ---
    void SetActivateHook(ActivateHook h);
    void ClearActivateHook();

private:
    ContentRegistry();
    // 依赖拓扑 DFS: 返回 false 表示循环依赖/缺失
    bool ActivateRecursive(const std::string& id, std::string* err,
                           std::vector<std::string>& visiting);

    std::unordered_map<std::string, ContentModule> modules_;
    std::unordered_map<std::string, ModuleState>   states_;
    int phase_caps_[4] = {30, 45, 60, 70};  // 与 Phase 枚举一一对应 (可 SetLevelCap 覆盖)
    ActivateHook hook_;
};

}  // namespace content
}  // namespace game
}  // namespace cami
