#pragma once
// ============================================================================
// game/content/lua_module_loader.h — Lua 模块懒加载骨架 (纯 STL)
// ----------------------------------------------------------------------------
// 每个玩法一个 Lua 模块, 启动只 require Tier 0 核心; 模块激活时懒 require
// (Lua require 自带缓存, 二次触发零解析成本)。
// LuaRuntime 抽象: MODULES=ON 接 Sol2 (lua_State); OFF 构建/单测用桩。
// 玩法逻辑全走 Lua, C++ 侧仅做加载调度 (红线合规)。
// ============================================================================
#include <set>
#include <string>

namespace cami {
namespace game {
namespace content {

// Lua 运行时抽象: 负责 require 一个模块文件并缓存其字节码。
class LuaRuntime {
public:
    virtual ~LuaRuntime() = default;
    // 加载并执行模块入口, 成功返回 true
    virtual bool Require(const std::string& module_path) = 0;
};

class LuaModuleLoader {
public:
    // 确保 Lua 模块已加载 (幂等)。未设置 runtime 时走桩路径 (仅记录, 返回 true)。
    bool EnsureLoaded(const std::string& lua_entry, std::string* err = nullptr);
    bool IsLoaded(const std::string& lua_entry) const;
    std::size_t LoadedCount() const;

    void SetRuntime(LuaRuntime* rt);
    // 模块热更: 清缓存后由上层重新 require
    void Invalidate(const std::string& lua_entry);

private:
    std::set<std::string> loaded_;  // 已 require 的模块路径
    LuaRuntime* rt_ = nullptr;
};

}  // namespace content
}  // namespace game
}  // namespace cami
