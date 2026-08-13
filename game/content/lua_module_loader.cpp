#include "game/content/lua_module_loader.h"

namespace cami {
namespace game {
namespace content {

bool LuaModuleLoader::EnsureLoaded(const std::string& lua_entry, std::string* err) {
    if (loaded_.count(lua_entry)) return true;  // Lua require 缓存语义: 幂等
    if (rt_) {
        if (!rt_->Require(lua_entry)) {
            if (err) *err = "lua require failed: '" + lua_entry + "'";
            return false;
        }
    }
    loaded_.insert(lua_entry);
    return true;
}

bool LuaModuleLoader::IsLoaded(const std::string& lua_entry) const {
    return loaded_.count(lua_entry) != 0;
}

std::size_t LuaModuleLoader::LoadedCount() const { return loaded_.size(); }

void LuaModuleLoader::SetRuntime(LuaRuntime* rt) { rt_ = rt; }

void LuaModuleLoader::Invalidate(const std::string& lua_entry) {
    loaded_.erase(lua_entry);  // 热更: 清缓存, 下次 EnsureLoaded 重新 require
}

}  // namespace content
}  // namespace game
}  // namespace cami
