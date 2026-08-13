// content_module_test.cpp — 内容模块化骨架单测 (纯 STL, OFF 构建可跑)
// 覆盖: 注册表完整性 / 依赖拓扑 / 激活幂等 / 双闸门 / 分阶段等级上限 /
//       ConfigManager LRU 淘汰 / LuaModuleLoader 懒加载与热更 / 循环依赖检测。
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "game/content/content_registry.h"
#include "game/content/config_manager.h"
#include "game/content/lua_module_loader.h"

using namespace cami::game::content;

// ============================ ContentRegistry ============================

TEST(ContentRegistryTest, BuiltinModulesComplete) {
    auto& reg = ContentRegistry::Instance();
    // 21 配置映射的关键模块全部注册, Tier 归属正确 (docs §6)
    const ContentModule* skills = reg.Find("skills");
    ASSERT_NE(skills, nullptr);
    EXPECT_EQ(skills->tier, 1);
    EXPECT_EQ(skills->level_gate, 2);
    EXPECT_EQ(skills->min_phase, Phase::kAlpha);

    const ContentModule* gems = reg.Find("gems");
    ASSERT_NE(gems, nullptr);
    EXPECT_EQ(gems->tier, 2);
    EXPECT_EQ(gems->level_gate, 20);
    EXPECT_EQ(gems->min_phase, Phase::kBeta);   // 宝石 = 阶段解锁
    EXPECT_EQ(gems->config_sets.size(), 1u);
    EXPECT_EQ(gems->config_sets[0], "config_gems");

    const ContentModule* enchanting = reg.Find("enchanting");
    ASSERT_NE(enchanting, nullptr);
    EXPECT_EQ(enchanting->min_phase, Phase::kBeta);  // 附魔 = 阶段解锁

    // Tier 0 核心常驻
    EXPECT_NE(reg.Find("world_zones"), nullptr);
    EXPECT_NE(reg.Find("quest_engine"), nullptr);
    // 未知模块
    EXPECT_EQ(reg.Find("nonexistent"), nullptr);
}

TEST(ContentRegistryTest, DependencyTopology) {
    auto& reg = ContentRegistry::Instance();
    const std::size_t before = reg.ActiveCount();

    // gems 依赖 items; 激活 gems 必须先激活 items (拓扑排序)
    ASSERT_TRUE(reg.EnsureLoaded("gems"));
    EXPECT_EQ(reg.State("gems"), ModuleState::kActive);
    EXPECT_EQ(reg.State("items"), ModuleState::kActive);   // 依赖先激活
    EXPECT_GE(reg.ActiveCount(), before + 1);
}

TEST(ContentRegistryTest, ActivateIdempotent) {
    auto& reg = ContentRegistry::Instance();
    // 用独立模块 (无依赖), 相对差值断言, 避免单例状态跨测试污染
    reg.Register({"test_idem", 0, {}, {}, "", 0, Phase::kAlpha, 0});
    const std::size_t before = reg.ActiveCount();
    ASSERT_TRUE(reg.EnsureLoaded("test_idem"));
    EXPECT_EQ(reg.ActiveCount(), before + 1);
    // 二次激活: 幂等, 不重复激活, 计数不变
    ASSERT_TRUE(reg.EnsureLoaded("test_idem"));
    EXPECT_EQ(reg.ActiveCount(), before + 1);
    EXPECT_EQ(reg.State("test_idem"), ModuleState::kActive);
}

TEST(ContentRegistryTest, UnknownModuleFails) {
    auto& reg = ContentRegistry::Instance();
    std::string err;
    EXPECT_FALSE(reg.EnsureLoaded("ghost_module", &err));
    EXPECT_FALSE(err.empty());
}

TEST(ContentRegistryTest, CircularDependencyDetected) {
    auto& reg = ContentRegistry::Instance();
    // 自定义循环: a -> b -> a
    reg.Register({"test_a", 0, {"test_b"}, {}, "", 0, Phase::kAlpha, 0});
    reg.Register({"test_b", 0, {"test_a"}, {}, "", 0, Phase::kAlpha, 0});
    std::string err;
    EXPECT_FALSE(reg.EnsureLoaded("test_a", &err));
    EXPECT_NE(err.find("circular"), std::string::npos);
}

TEST(ContentRegistryTest, DualGateUnlock) {
    auto& reg = ContentRegistry::Instance();
    // gems: min_phase=Beta, level_gate=20
    // 内容闸门: Alpha 阶段不可见 (即使等级够)
    EXPECT_FALSE(reg.IsUnlocked("gems", 30, Phase::kAlpha));
    // 个人闸门: Beta 阶段但等级不够
    EXPECT_FALSE(reg.IsUnlocked("gems", 10, Phase::kBeta));
    // 双达标
    EXPECT_TRUE(reg.IsUnlocked("gems", 20, Phase::kBeta));
    EXPECT_TRUE(reg.IsUnlocked("gems", 45, Phase::kRelease));
    // 超过当前阶段等级上限不可见 (Beta 上限 45)
    EXPECT_FALSE(reg.IsUnlocked("gems", 60, Phase::kBeta));
    // 未注册模块
    EXPECT_FALSE(reg.IsUnlocked("ghost", 99, Phase::kRelease));
}

TEST(ContentRegistryTest, PhaseLevelCaps) {
    auto& reg = ContentRegistry::Instance();
    // 2026-08-12 决策: 30/45/60/70 四段
    EXPECT_EQ(reg.LevelCap(Phase::kAlpha), 30);
    EXPECT_EQ(reg.LevelCap(Phase::kBeta), 45);
    EXPECT_EQ(reg.LevelCap(Phase::kGamma), 60);
    EXPECT_EQ(reg.LevelCap(Phase::kRelease), 70);
    // 后期扩展接口: 覆盖 (如加 80 级阶段前先调整)
    reg.SetLevelCap(Phase::kRelease, 80);
    EXPECT_EQ(reg.LevelCap(Phase::kRelease), 80);
    reg.SetLevelCap(Phase::kRelease, 70);  // 还原
}

// ============================ ConfigManager ============================

namespace {
// 内存桩 ConfigSource: 每配置集固定字节, 记录加载调用
class StubSource : public ConfigSource {
public:
    explicit StubSource(std::size_t bytes) : bytes_(bytes) {}
    bool Load(const std::string&, std::size_t& out) override {
        ++calls;
        out = bytes_;
        return true;
    }
    std::size_t calls = 0;

private:
    std::size_t bytes_;
};
}  // namespace

TEST(ConfigManagerTest, LazyLoadAndIdempotent) {
    StubSource src(1024);
    ConfigManager mgr(1u << 20);  // 1MB
    mgr.SetSource(&src);

    EXPECT_FALSE(mgr.IsLoaded("config_gems"));
    ASSERT_TRUE(mgr.EnsureLoaded("config_gems"));
    EXPECT_TRUE(mgr.IsLoaded("config_gems"));
    EXPECT_EQ(mgr.CacheEntries(), 1u);
    EXPECT_EQ(mgr.CacheBytes(), 1024u);
    EXPECT_EQ(src.calls, 1u);

    // 幂等: 二次加载不重复调 source
    ASSERT_TRUE(mgr.EnsureLoaded("config_gems"));
    EXPECT_EQ(src.calls, 1u);
    EXPECT_EQ(mgr.CacheEntries(), 1u);
}

TEST(ConfigManagerTest, LruEviction) {
    StubSource src(100);  // 每配置 100B
    ConfigManager mgr(250);  // 容量 250B: 最多 2 条
    mgr.SetSource(&src);

    ASSERT_TRUE(mgr.EnsureLoaded("cfg_a"));  // 100
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_b"));  // 200
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_c"));  // 300 > 250 → 淘汰 cfg_a
    EXPECT_FALSE(mgr.IsLoaded("cfg_a"));
    EXPECT_TRUE(mgr.IsLoaded("cfg_b"));
    EXPECT_TRUE(mgr.IsLoaded("cfg_c"));
    EXPECT_EQ(mgr.CacheEntries(), 2u);
    EXPECT_LE(mgr.CacheBytes(), 250u);
}

TEST(ConfigManagerTest, TouchRefreshesLru) {
    StubSource src(100);
    ConfigManager mgr(200);
    mgr.SetSource(&src);
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_a"));
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_b"));   // 满 200
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_a"));   // touch a → 最近使用
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_c"));   // 需淘汰: 应淘汰 b (最久未用)
    EXPECT_TRUE(mgr.IsLoaded("cfg_a"));
    EXPECT_FALSE(mgr.IsLoaded("cfg_b"));
    EXPECT_TRUE(mgr.IsLoaded("cfg_c"));
}

TEST(ConfigManagerTest, NoSourceFails) {
    ConfigManager mgr;
    std::string err;
    EXPECT_FALSE(mgr.EnsureLoaded("config_gems", &err));
    EXPECT_FALSE(err.empty());
}

TEST(ConfigManagerTest, EvictManual) {
    StubSource src(100);
    ConfigManager mgr(1u << 20);
    mgr.SetSource(&src);
    ASSERT_TRUE(mgr.EnsureLoaded("cfg_x"));
    EXPECT_TRUE(mgr.Evict("cfg_x"));
    EXPECT_FALSE(mgr.IsLoaded("cfg_x"));
    EXPECT_FALSE(mgr.Evict("cfg_x"));  // 二次淘汰 false
}

// ============================ LuaModuleLoader ============================

namespace {
class StubRuntime : public LuaRuntime {
public:
    bool Require(const std::string&) override {
        ++require_count;
        return true;
    }
    std::size_t require_count = 0;
};
class FailingRuntime : public LuaRuntime {
public:
    bool Require(const std::string&) override { return false; }
};
}  // namespace

TEST(LuaModuleLoaderTest, LazyRequire) {
    StubRuntime rt;
    LuaModuleLoader loader;
    loader.SetRuntime(&rt);

    EXPECT_EQ(loader.LoadedCount(), 0u);
    ASSERT_TRUE(loader.EnsureLoaded("systems/gems/init.lua"));
    EXPECT_EQ(loader.LoadedCount(), 1u);
    EXPECT_TRUE(loader.IsLoaded("systems/gems/init.lua"));
    EXPECT_EQ(rt.require_count, 1u);

    // Lua require 缓存: 二次零成本
    ASSERT_TRUE(loader.EnsureLoaded("systems/gems/init.lua"));
    EXPECT_EQ(rt.require_count, 1u);
}

TEST(LuaModuleLoaderTest, HotReloadInvalidate) {
    StubRuntime rt;
    LuaModuleLoader loader;
    loader.SetRuntime(&rt);
    ASSERT_TRUE(loader.EnsureLoaded("systems/skills/init.lua"));
    EXPECT_EQ(rt.require_count, 1u);

    loader.Invalidate("systems/skills/init.lua");  // 热更: 清缓存
    EXPECT_FALSE(loader.IsLoaded("systems/skills/init.lua"));
    ASSERT_TRUE(loader.EnsureLoaded("systems/skills/init.lua"));  // 重新 require
    EXPECT_EQ(rt.require_count, 2u);
}

TEST(LuaModuleLoaderTest, RuntimeFailurePropagates) {
    FailingRuntime rt;
    LuaModuleLoader loader;
    loader.SetRuntime(&rt);
    std::string err;
    EXPECT_FALSE(loader.EnsureLoaded("systems/broken/init.lua", &err));
    EXPECT_FALSE(err.empty());
    EXPECT_EQ(loader.LoadedCount(), 0u);
}

TEST(LuaModuleLoaderTest, NoRuntimeStubPath) {
    // 未注入 runtime: 桩路径 (仅记录, 返回 true) — OFF 构建语义
    LuaModuleLoader loader;
    EXPECT_TRUE(loader.EnsureLoaded("systems/gems/init.lua"));
    EXPECT_TRUE(loader.IsLoaded("systems/gems/init.lua"));
    EXPECT_EQ(loader.LoadedCount(), 1u);
}
