// client/resource/tests/resource_test.cpp —— TASK-036 单元测试（ctest 'Resource.Suite'）。
//
// 所有输出走 mmo::core::test（红线：禁止 std::cout/printf/std::cerr）。

#include "test_print.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "mmo/client/resource/quality_preset.h"
#include "mmo/client/resource/resource_manager.h"
#include "mmo/client/resource/scene_streamer.h"

namespace tprint = ::mmo::core::test;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            ++g_passed;                                                      \
            tprint::LineFmt("[PASS] %s\n", name);                            \
        } else {                                                             \
            ++g_failed;                                                      \
            tprint::LineFmt("[FAIL] %s (line %d)\n", name, __LINE__);        \
        }                                                                    \
    } while (0)

using namespace mmo::client::resource;

static void TestQualityPreset() {
    auto lp = LoadPresets("config/client/quality.json");
    CHECK(static_cast<bool>(lp), "LoadPresets(config/client/quality.json)");

    auto low = GetPreset(QualityLevel::Low);
    CHECK(static_cast<bool>(low), "GetPreset(Low)");
    CHECK(low.Value().texture_max_size == 512u, "low.texture_max_size==512");
    CHECK(low.Value().max_visible_entities == 50u, "low.max_visible_entities==50");
    CHECK(low.Value().texture_budget_bytes == 256ULL * 1024 * 1024, "low.tex_budget==256MB");
    CHECK(low.Value().mesh_budget_bytes == 128ULL * 1024 * 1024, "low.mesh_budget==128MB");
    CHECK(low.Value().chunk_radius == 1u, "low.chunk_radius==1");
    CHECK(low.Value().lod_bias == 1.0f, "low.lod_bias==1.0");

    auto med = GetPreset(QualityLevel::Medium);
    CHECK(static_cast<bool>(med), "GetPreset(Medium)");
    CHECK(med.Value().texture_max_size == 1024u, "med.texture_max_size==1024");
    CHECK(med.Value().max_visible_entities == 150u, "med.max_visible_entities==150");
    CHECK(med.Value().texture_budget_bytes == 512ULL * 1024 * 1024, "med.tex_budget==512MB");

    auto high = GetPreset(QualityLevel::High);
    CHECK(static_cast<bool>(high), "GetPreset(High)");
    CHECK(high.Value().texture_max_size == 2048u, "high.texture_max_size==2048");
    CHECK(high.Value().max_visible_entities == 300u, "high.max_visible_entities==300");
    CHECK(high.Value().texture_budget_bytes == 1024ULL * 1024 * 1024, "high.tex_budget==1GB");
    CHECK(high.Value().mesh_budget_bytes == 512ULL * 1024 * 1024, "high.mesh_budget==512MB");
}

static void TestRefCounting() {
    ResourceManager rm;
    auto ir = rm.Init(GetPreset(QualityLevel::Low).Value());
    CHECK(static_cast<bool>(ir), "rm.Init(Low)");

    auto h1 = rm.LoadAsync("texA", ResourceType::Texture);
    auto h2 = rm.LoadAsync("texA", ResourceType::Texture);  // 同 uri -> 同句柄 + 引用+1
    CHECK(static_cast<bool>(h1) && static_cast<bool>(h2), "load texA twice");
    CHECK(h1.Value().id == h2.Value().id, "same uri -> same handle id");
    ResourceStats s = rm.Stats();
    CHECK(s.handle_count == 1u, "dedup: handle_count==1");

    auto u1 = rm.Unload(h1.Value());
    CHECK(static_cast<bool>(u1), "unload h1");
    CHECK(rm.Stats().handle_count == 1u, "handle still alive after 1 unload");
    auto u2 = rm.Unload(h2.Value());
    CHECK(static_cast<bool>(u2), "unload h2");
    CHECK(rm.Stats().handle_count == 1u, "handle becomes LRU candidate, not freed yet");

    // 重复的无效 Unload 幂等
    auto u3 = rm.Unload(ResourceHandle{999999u});
    CHECK(!static_cast<bool>(u3), "unload unknown handle -> Fail(NOT_FOUND)");
}

static void TestLruEviction() {
    QualityPreset tiny = GetPreset(QualityLevel::Low).Value();
    tiny.texture_budget_bytes = 2ULL * 1024 * 1024;  // 2MB，每 Low 纹理 1MB
    ResourceManager rm;
    CHECK(static_cast<bool>(rm.Init(tiny)), "rm.Init(tiny 2MB tex budget)");

    // 加载 10 个不同纹理并立即释放引用，触发 LRU 回收（每纹理 1MB，预算 2MB）
    for (int i = 0; i < 10; ++i) {
        auto h = rm.LoadAsync("lru" + std::to_string(i), ResourceType::Texture);
        CHECK(static_cast<bool>(h), "lru load");
        rm.Unload(h.Value());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ResourceStats s = rm.Stats();
    CHECK(s.texture_bytes <= tiny.texture_budget_bytes, "texture_bytes <= 2MB budget");
    CHECK(s.lru_evictions > 0u, "lru_evictions > 0");
    CHECK(s.texture_count <= 2u, "at most 2 textures retained (within 2MB)");

    // 统计一致性
    CHECK(std::abs(s.ram_mb - static_cast<double>(s.ram_bytes) / 1048576.0) < 1e-6, "ram_mb == ram_bytes/1MB");
}

static void TestIndependentBudgets() {
    QualityPreset p = GetPreset(QualityLevel::Low).Value();
    p.texture_budget_bytes = 2ULL * 1024 * 1024;   // 纹理预算 2MB
    p.mesh_budget_bytes = 64ULL * 1024 * 1024;      // 网格预算 64MB（独立）
    ResourceManager rm;
    CHECK(static_cast<bool>(rm.Init(p)), "rm.Init(tex2MB/mesh64MB)");

    for (int i = 0; i < 30; ++i) {
        auto h = rm.LoadAsync("tb" + std::to_string(i), ResourceType::Texture);
        CHECK(static_cast<bool>(h), "tex load");
        rm.Unload(h.Value());
    }
    for (int i = 0; i < 120; ++i) {
        auto h = rm.LoadAsync("mb" + std::to_string(i), ResourceType::Mesh);
        CHECK(static_cast<bool>(h), "mesh load");
        rm.Unload(h.Value());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    ResourceStats s = rm.Stats();
    CHECK(s.texture_bytes <= 2ULL * 1024 * 1024, "texture within its own 2MB budget");
    CHECK(s.mesh_bytes <= 64ULL * 1024 * 1024, "mesh within its own 64MB budget");
    CHECK(s.mesh_bytes > p.texture_budget_bytes, "mesh not starved by texture budget (independent)");
}

static void TestAsyncNonBlocking() {
    ResourceManager rm;
    CHECK(static_cast<bool>(rm.Init(GetPreset(QualityLevel::Low).Value())), "rm.Init");
    auto h = rm.LoadAsync("async1", ResourceType::Texture);
    CHECK(static_cast<bool>(h), "async load returns immediately");
    ResourceStats sA = rm.Stats();
    CHECK(sA.handle_count == 1u, "handle exists immediately (pending)");
    CHECK(sA.texture_bytes == 0u, "not committed yet (pending)");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ResourceStats sB = rm.Stats();
    CHECK(sB.texture_bytes > 0u, "committed after worker completes");
    CHECK(sB.load_ms_p95 >= 0.0, "load_ms_p95 computed");
}

static void TestCacheHit() {
    ResourceManager rm;
    CHECK(static_cast<bool>(rm.Init(GetPreset(QualityLevel::Low).Value())), "rm.Init");
    auto h1 = rm.LoadAsync("hitme", ResourceType::Texture);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::uint64_t before = rm.Stats().total_requests;
    auto h2 = rm.LoadAsync("hitme", ResourceType::Texture);  // 命中
    ResourceStats s = rm.Stats();
    CHECK(s.total_requests == before + 1u, "second request counted");
    CHECK(s.cache_hits >= 1u, "cache hit recorded");
    CHECK(s.cache_hit_rate > 0.0, "cache_hit_rate > 0");
    (void)h1; (void)h2;
}

static void TestChunkStreaming() {
    SceneStreamer st;
    SceneStreamer::Config cfg;
    cfg.chunk_size = 64.0f;
    cfg.load_radius = 1;
    cfg.unload_delay_seconds = 2;
    cfg.max_pending_loads = 4;
    st.Configure(cfg);

    FrameContext f;
    f.dt_ms = 16.0;
    f.frame_index = 0;
    mmo::client::Vec3 p{0.0f, 0.0f, 0.0f};
    st.Update(p, f);
    for (int i = 0; i < 30; ++i) { f.frame_index++; st.Update(p, f); }

    StreamingStats s0 = st.Stats();
    CHECK(s0.loaded_chunks >= 1u, "at least current chunk loaded");
    CHECK(s0.loaded_chunks <= 9u, "at most (2r+1)^2==9 chunks for r=1");
    CHECK(s0.pending_loads == 0u, "all loads completed");

    // 瞬移到远处：旧 chunk 应在延迟后卸载，新区域 chunk 加载
    mmo::client::Vec3 far{10000.0f, 0.0f, 10000.0f};
    for (int i = 0; i < 200; ++i) { f.frame_index++; f.dt_ms = 16.0; st.Update(far, f); }
    StreamingStats s1 = st.Stats();
    CHECK(s1.unload_count > 0u, "old chunks unloaded after delay");
    CHECK(s1.loaded_chunks >= 1u, "new area chunk loaded");
}

static void TestHysteresisAntiStorm() {
    SceneStreamer st;
    SceneStreamer::Config cfg;
    cfg.chunk_size = 64.0f;
    cfg.load_radius = 1;
    cfg.unload_delay_seconds = 5;     // 长延迟，迟滞生效
    cfg.max_pending_loads = 4;
    st.Configure(cfg);

    FrameContext f;
    f.dt_ms = 16.0;
    // 在 chunk 边界（x≈0）附近高频来回穿越 100 次
    for (int i = 0; i < 100; ++i) {
        f.frame_index++;
        float x = (i % 2 == 0) ? 10.0f : -10.0f;
        mmo::client::Vec3 p{x, 0.0f, 0.0f};
        st.Update(p, f);
    }
    StreamingStats s = st.Stats();
    // 触及的 chunk 集合有限（cx∈{-2..1}, cz∈{-1..1} → ≤12），不应爆炸式重复加载
    CHECK(s.requested_loads <= 16u, "no load storm (bounded requests)");  // 迟滞生效
    CHECK(s.loaded_chunks <= 12u, "loaded chunk count bounded");
}

static void TestQualitySwitch() {
    ResourceManager rm;
    auto high = GetPreset(QualityLevel::High);
    CHECK(static_cast<bool>(high), "GetPreset(High)");
    CHECK(static_cast<bool>(rm.Init(high.Value())), "rm.Init(High)");

    // 轮询辅助：最多等 5s 直到谓词为真（避免 Debug/Release 速度差异导致的不稳定）。
    auto WaitUntil = [](auto pred) {
        for (int i = 0; i < 500; ++i) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    };

    std::vector<ResourceHandle> hs;
    for (int i = 0; i < 20; ++i) {
        auto h = rm.LoadAsync("hq" + std::to_string(i), ResourceType::Texture);  // 每 High 纹理 16MB
        CHECK(static_cast<bool>(h), "hq load");
        hs.push_back(h.Value());
    }
    // 等全部 20 张 High 纹理提交（High 预算 1GB，不会被回收）
    WaitUntil([&]() { return rm.Stats().texture_count == 20u; });
    size_t highBytes = rm.Stats().texture_bytes;
    CHECK(highBytes > 256ULL * 1024 * 1024, "high textures exceed Low 256MB budget");

    auto sq = rm.SetQuality(QualityLevel::Low);
    CHECK(static_cast<bool>(sq), "SetQuality(Low)");

    // 释放引用后，LRU 应按 Low 预算（256MB）回收
    for (const auto& h : hs) rm.Unload(h);
    const bool reclaimed = WaitUntil([&]() { return rm.Stats().texture_bytes <= 256ULL * 1024 * 1024; });

    ResourceStats sl = rm.Stats();
    CHECK(reclaimed, "after switch+release within Low 256MB");
    CHECK(sl.texture_bytes < highBytes, "texture bytes decreased after switch to Low");
}

int main() {
    tprint::Line("=== TASK-036 Resource / Low Spec System tests ===\n");
    TestQualityPreset();
    TestRefCounting();
    TestLruEviction();
    TestIndependentBudgets();
    TestAsyncNonBlocking();
    TestCacheHit();
    TestChunkStreaming();
    TestHysteresisAntiStorm();
    TestQualitySwitch();

    tprint::LineFmt("SUMMARY passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
