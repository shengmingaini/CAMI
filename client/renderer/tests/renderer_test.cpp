// client/renderer/tests/renderer_test.cpp — TASK-035 Renderer 单元测试
//
// 覆盖任务书验收口径：
//   §16 Unit Test：Camera 视锥构造与剔除 / LOD 三级 / 材质合批 / UI 图集 / 画质切换 / 统计准确
//   §17 Integration：1000 静态 + 200 动态 + 50 UI，Low 档 DrawCall<300
//   §20 Acceptance 8 条核心（剔除 / LOD / 合批 / UI / 画质 / 统计 / 双构建 / ctest 见构建）
//
// 输出：走 mmo::core::test 通道（禁止 cout/printf）。

#include "mmo/client/client_world.h"
#include "mmo/client/render/renderer.h"

#include "test_print.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace tprint = ::mmo::core::test;
namespace render = mmo::client::render;
using namespace mmo::client;
using namespace mmo::client::render;

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

// 构造一个含若干实体的单帧世界快照（全部 ts=0，render_time=0 即取当前姿态）。
static ClientWorld MakeWorld(const std::vector<std::pair<EntityId, Vec3>>& ents,
                             std::int64_t ts = 0) {
    ClientWorld w;
    WorldSnapshot s;
    s.server_time_ms = ts;
    for (auto& kv : ents) {
        EntityPose p;
        p.id = kv.first;
        p.pos = kv.second;
        p.ts_ms = ts;
        s.entities.push_back(p);
    }
    w.ApplySnapshot(s);
    return w;
}

// 构造 Vec3 的安全助手：避免在大括号初始化里写带一元负号的复杂表达式（GCC 解析坑）。
static Vec3 MkV(float x, float y, float z) {
    Vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

static Camera MakeCamPersp(float fov = 60.0f, float aspect = 16.0f / 9.0f,
                           float near_z = 0.1f, float far_z = 1000.0f) {
    Camera cam;
    cam.SetPerspective(fov, aspect, near_z, far_z);
    cam.LookAt(Vec3{0, 0, 0}, Vec3{0, 0, -1}, Vec3{0, 1, 0});
    return cam;
}

int main() {
    // --- A. 视锥剔除 + LOD 三级 + 统计真实（§20 #2/#3/#4/#6）---
    {
        Renderer r;
        (void)r.Init(RendererConfig{});
        r.RegisterMesh(1, 1000, 1024);
        r.RegisterMesh(2, 1000, 1024);
        r.RegisterMesh(3, 1000, 1024);
        r.RegisterTexture(1, 2048);
        r.RegisterTexture(2, 2048);
        auto mat = r.CreateMaterial(MaterialDesc{}).Value();

        ClientWorld w = MakeWorld({
            {1, Vec3{0, 0, -10}},     // 近：LOD factor 1.0
            {2, Vec3{0, 0, -50}},     // 中：LOD factor 0.5
            {3, Vec3{0, 0, -150}},    // 远：LOD factor 0.25
            {99, Vec3{1000, 0, -50}}, // 视锥外（极右侧）-> 剔除
        });

        Renderable ra; ra.id = 1; ra.mesh = 1; ra.material = mat; ra.bounding_radius = 1.0f;
        ra.lod_near = 30; ra.lod_mid = 80;
        Renderable rb; rb.id = 2; rb.mesh = 2; rb.material = mat; rb.bounding_radius = 1.0f;
        rb.lod_near = 30; rb.lod_mid = 80;
        Renderable rc; rc.id = 3; rc.mesh = 3; rc.material = mat; rb.bounding_radius = 1.0f;
        rc.lod_near = 30; rc.lod_mid = 80;
        Renderable rd; rd.id = 99; rd.mesh = 1; rd.material = mat; rd.bounding_radius = 1.0f;
        rd.lod_near = 30; rd.lod_mid = 80;
        r.AddRenderable(ra); r.AddRenderable(rb); r.AddRenderable(rc); r.AddRenderable(rd);

        auto cam = MakeCamPersp();
        auto res = r.RenderFrame(w, cam, 0);
        CHECK(res.HasValue(), "A_render_ok");
        if (res.HasValue()) {
            const RenderStats& s = res.Value();
            CHECK(s.visible_entities == 3, "A_visible_3");
            CHECK(s.culled_entities == 1, "A_culled_1");
            CHECK(s.triangles == 1750, "A_lod_triangles");  // 1000 + 500 + 250
            CHECK(s.materials == 1, "A_materials_1");
            CHECK(s.draw_calls == 1, "A_drawcalls_1");
            CHECK(s.shader_switches == 0, "A_shader_switches_0");
            CHECK(s.texture_memory_bytes == 4096, "A_tex_bytes");
            CHECK(s.mesh_memory_bytes == 3072, "A_mesh_bytes");
            CHECK(s.cpu_ms >= 0.0f, "A_cpu_ms_nonneg");
            CHECK(s.gpu_ms >= 0.0f, "A_gpu_ms_nonneg");
        }
    }

    // --- B. 静态合批：同材质多实例 = 1 Draw Call；不同材质 = 各 1 ---
    {
        Renderer r;
        (void)r.Init(RendererConfig{});
        r.RegisterMesh(1, 2000, 512);
        auto matA = r.CreateMaterial(MaterialDesc{}).Value();
        auto matB = r.CreateMaterial(MaterialDesc{}).Value();

        std::vector<std::pair<EntityId, Vec3>> ents;
        for (int i = 1; i <= 10; ++i)
            ents.push_back({(EntityId)i, MkV(0.0f, 0.0f, -(float)(20 + i))});
        ClientWorld w = MakeWorld(ents);

        for (int i = 1; i <= 10; ++i) {
            Renderable rr; rr.id = i; rr.mesh = 1; rr.material = matA; rr.bounding_radius = 1.0f;
            rr.lod_near = 1000.0f; rr.lod_mid = 2000.0f;  // 全部落在 near 带 -> LOD factor 1.0
            r.AddRenderable(rr);
        }
        auto cam = MakeCamPersp();
        auto res = r.RenderFrame(w, cam, 0);
        CHECK(res.HasValue(), "B_render_ok");
        if (res.HasValue()) {
            const RenderStats& s = res.Value();
            CHECK(s.visible_entities == 10, "B_visible_10");
            CHECK(s.draw_calls == 1, "B_batch_1_drawcall");
            CHECK(s.triangles == 10 * 2000, "B_triangles_sum");
        }

        // 两材质交替 -> 2 Draw Calls
        Renderer r2;
        (void)r2.Init(RendererConfig{});
        r2.RegisterMesh(1, 2000, 512);
        auto matA2 = r2.CreateMaterial(MaterialDesc{}).Value();
        auto matB2 = r2.CreateMaterial(MaterialDesc{}).Value();
        for (int i = 1; i <= 10; ++i) {
            Renderable rr; rr.id = i; rr.mesh = 1;
            rr.material = (i % 2 == 0) ? matB2 : matA2;
            rr.bounding_radius = 1.0f;
            r2.AddRenderable(rr);
        }
        auto res2 = r2.RenderFrame(w, cam, 0);
        CHECK(res2.HasValue(), "B2_render_ok");
        if (res2.HasValue()) {
            CHECK(res2.Value().draw_calls == 2, "B2_batch_2_drawcalls");
        }
    }

    // --- C. UI 同图集批量（默认 1 Draw Call；上限 20）---
    {
        Renderer r;
        (void)r.Init(RendererConfig{});
        r.RegisterMesh(1, 100, 64);
        auto mat = r.CreateMaterial(MaterialDesc{}).Value();

        ClientWorld w = MakeWorld({{1, Vec3{0, 0, -20}}});
        Renderable rr; rr.id = 1; rr.mesh = 1; rr.material = mat; rr.bounding_radius = 1.0f;
        r.AddRenderable(rr);
        auto cam = MakeCamPersp();

        // 同图集多次批量 -> 1 个 UI Draw Call；世界 1 + UI 1 = 2
        r.AddUIBatch(20, mat);
        r.AddUIBatch(15, mat);
        auto res = r.RenderFrame(w, cam, 0);
        CHECK(res.HasValue(), "C_render_ok");
        if (res.HasValue()) {
            CHECK(res.Value().draw_calls == 2, "C_ui_1_drawcall");
        }

        // 超过 20 个不同图集 -> 截断到 20
        Renderer r2;
        (void)r2.Init(RendererConfig{});
        r2.RegisterMesh(1, 100, 64);
        for (int i = 0; i < 25; ++i) {
            auto a = r2.CreateMaterial(MaterialDesc{}).Value();
            r2.AddUIBatch(1, a);
        }
        auto res2 = r2.RenderFrame(w, cam, 0);
        CHECK(res2.HasValue(), "C2_render_ok");
        if (res2.HasValue()) {
            CHECK(res2.Value().draw_calls == 20, "C2_ui_cap_20");
        }
    }

    // --- D. 画质热切换（§20 #5）---
    {
        Renderer r;
        (void)r.Init(RendererConfig{});
        CHECK(r.CurrentQuality() == QualityLevel::Low, "D_default_low");
        auto ok = r.SetQuality(QualityLevel::High);
        CHECK(ok.HasValue(), "D_setquality_ok");
        CHECK(r.CurrentQuality() == QualityLevel::High, "D_current_high");
    }

    // --- E. 空世界 / 全剔除边界 ---
    {
        Renderer r;
        (void)r.Init(RendererConfig{});
        r.RegisterMesh(1, 100, 64);
        auto mat = r.CreateMaterial(MaterialDesc{}).Value();

        ClientWorld w = MakeWorld({});  // 无实体
        Renderable rr; rr.id = 1; rr.mesh = 1; rr.material = mat; rr.bounding_radius = 1.0f;
        r.AddRenderable(rr);  // 实体不在世界里 -> 不被渲染
        auto cam = MakeCamPersp();
        auto res = r.RenderFrame(w, cam, 0);
        CHECK(res.HasValue(), "E_render_ok");
        if (res.HasValue()) {
            const RenderStats& s = res.Value();
            CHECK(s.visible_entities == 0, "E_visible_0");
            CHECK(s.culled_entities == 0, "E_culled_0");
            CHECK(s.draw_calls == 0, "E_drawcalls_0");
            CHECK(s.triangles == 0, "E_triangles_0");
        }
    }

    // --- F. Camera 视锥剔除直接验证（§16）---
    {
        auto cam = MakeCamPersp();
        CHECK(cam.IsVisible(Vec3{0, 0, -50}, 1.0f), "F_front_visible");
        CHECK(!cam.IsVisible(Vec3{0, 0, 50}, 1.0f), "F_behind_culled");
        CHECK(!cam.IsVisible(Vec3{1000, 0, -50}, 1.0f), "F_side_culled");
        CHECK(cam.IsVisible(Vec3{5, 0, -10}, 1.0f), "F_near_center_visible");
    }

    // --- G. §17 集成：1000 静态 + 200 动态 + 50 UI，Low 档 DrawCall<300 ---
    {
        Renderer r;
        RendererConfig cfg; cfg.quality = QualityLevel::Low; cfg.max_draw_calls = 300;
        (void)r.Init(cfg);
        const int kMeshes = 64, kMats = 50, kTex = 40;
        for (int i = 1; i <= kMeshes; ++i) r.RegisterMesh(i, 180, 1024 * 1024);  // 1MB/网格
        for (int i = 1; i <= kTex; ++i)    r.RegisterTexture(i, 1024 * 1024);    // 1MB/纹理
        std::vector<MaterialId> mats;
        for (int i = 1; i <= kMats; ++i) mats.push_back(r.CreateMaterial(MaterialDesc{}).Value());

        const int kTotal = 1200;  // 1000 静态 + 200 动态
        std::vector<std::pair<EntityId, Vec3>> ents;
        for (int i = 1; i <= kTotal; ++i) {
            const float ang = (float)i * 0.137f;
            const float rad = 20.0f + (float)(i % 100) * 2.0f;
            Vec3 pos = MkV(std::cos(ang) * rad, 0.0f, -(std::fabs(std::sin(ang) * rad) + 5.0f));
            ents.push_back({(EntityId)i, pos});
        }
        ClientWorld w = MakeWorld(ents);
        for (int i = 1; i <= kTotal; ++i) {
            Renderable rr;
            rr.id = i;
            rr.mesh = (i % kMeshes) + 1;
            rr.material = mats[(i - 1) % kMats];
            rr.bounding_radius = 1.0f;
            rr.lod_near = 30; rr.lod_mid = 80;
            r.AddRenderable(rr);
        }
        // 50 个 UI 元素（复用材质作图集 -> 去重后最多 kMats 个，截断到 20）
        for (int i = 0; i < 50; ++i) r.AddUIBatch(1, mats[i % kMats]);

        auto cam = MakeCamPersp();
        auto res = r.RenderFrame(w, cam, 0);
        CHECK(res.HasValue(), "G_render_ok");
        if (res.HasValue()) {
            const RenderStats& s = res.Value();
            CHECK(s.visible_entities > 0, "G_some_visible");
            CHECK(s.draw_calls <= cfg.max_draw_calls, "G_drawcalls_within_budget");
            CHECK(s.draw_calls < 300, "G_drawcalls_lt_300");
            CHECK(s.triangles <= 300000, "G_triangles_le_300k");
            CHECK(s.texture_memory_bytes <= 512ULL * 1024 * 1024, "G_tex_mem_le_512mb");
            tprint::LineFmt("  [info] G draw_calls=%u triangles=%u tex_mb=%llu mesh_mb=%llu\n",
                            (unsigned)s.draw_calls, (unsigned)s.triangles,
                            (unsigned long long)(s.texture_memory_bytes / (1024 * 1024)),
                            (unsigned long long)(s.mesh_memory_bytes / (1024 * 1024)));
        }
    }

    tprint::LineFmt("SUMMARY passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
