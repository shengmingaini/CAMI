// client/renderer/benchmark/render_bench.cpp — TASK-035 Renderer Benchmark
//
// 输出 bench/render_<quality>.txt（low 档即 render_low.txt，见 §24 验收）。
// 全部 key=value 由真实 RenderFrame 执行得出，禁止估算。
//
// 用法：bin/render_bench --scene test_scene --quality low --duration 600
//
// 红线：输出走 mmo::core::test 通道 + std::FILE*(vsnprintf/fwrite)，禁止 cout/printf。

#include "mmo/client/client_world.h"
#include "mmo/client/render/renderer.h"

#include "test_print.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

namespace tprint = ::mmo::core::test;
using namespace mmo::client;
using namespace mmo::client::render;

namespace {

struct Args {
    std::string quality = "low";
    double      duration_s = 10.0;
};

Args Parse(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--quality" && i + 1 < argc) a.quality = argv[++i];
        else if (s == "--duration" && i + 1 < argc) a.duration_s = std::atof(argv[++i]);
        else if (s == "--scene" && i + 1 < argc) ++i;  // 预留，本 headless 后端忽略
    }
    return a;
}

double BenchNowMs() {
    using namespace std::chrono;
    return duration_cast<duration<double, std::milli>>(
               steady_clock::now().time_since_epoch())
        .count();
}

double Percentile95(std::vector<double>& v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    std::size_t idx = static_cast<std::size_t>(0.95 * (v.size() - 1) + 0.5);
    if (idx >= v.size()) idx = v.size() - 1;
    return v[idx];
}

// 与测试一致的场景构造：单帧世界快照（ts=0）。
ClientWorld MakeWorld(const std::vector<std::pair<EntityId, Vec3>>& ents,
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

// 构造 Vec3 的安全助手：避免在大括号初始化里写带一元负号的复杂表达式。
Vec3 MkV(float x, float y, float z) {
    Vec3 v;
    v.x = x; v.y = y; v.z = z;
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = Parse(argc, argv);

    QualityLevel q = QualityLevel::Low;
    if (a.quality == "medium") q = QualityLevel::Medium;
    else if (a.quality == "high") q = QualityLevel::High;

    Renderer r;
    RendererConfig cfg;
    cfg.quality = q;
    cfg.max_draw_calls = 300;
    (void)r.Init(cfg);

    const int kMeshes = 64, kMats = 50, kTex = 40;
    for (int i = 1; i <= kMeshes; ++i) r.RegisterMesh(i, 180, 1024 * 1024);  // 1MB/网格
    for (int i = 1; i <= kTex; ++i)    r.RegisterTexture(i, 1024 * 1024);    // 1MB/纹理
    std::vector<MaterialId> mats;
    for (int i = 1; i <= kMats; ++i) mats.push_back(r.CreateMaterial(MaterialDesc{}).Value());

    const int kTotal = 1200;  // 1000 静态 + 200 动态（本 headless 基准统一处理）
    std::vector<std::pair<EntityId, Vec3>> ents;
    for (int i = 1; i <= kTotal; ++i) {
        const float ang = static_cast<float>(i) * 0.137f;
        const float rad = 20.0f + static_cast<float>(i % 100) * 2.0f;
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
        rr.lod_near = 30;
        rr.lod_mid = 80;
        r.AddRenderable(rr);
    }
    for (int i = 0; i < 50; ++i) r.AddUIBatch(1, mats[i % kMats]);

    Camera cam;
    cam.SetPerspective(60.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    cam.LookAt(Vec3{0, 0, 0}, Vec3{0, 0, -1}, Vec3{0, 1, 0});

    // 预热
    for (int i = 0; i < 30; ++i) { auto _ = r.RenderFrame(w, cam, 0); (void)_; }

    // 计时运行
    std::vector<double> frame_ms;
    const double t_end = BenchNowMs() + a.duration_s * 1000.0;
    std::int64_t rt = 0;
    double cpu_acc = 0.0, gpu_acc = 0.0;
    int n = 0;
    while (BenchNowMs() < t_end) {
        const double f0 = BenchNowMs();
        auto res = r.RenderFrame(w, cam, rt);
        const double f1 = BenchNowMs();
        if (!res.HasValue()) break;
        frame_ms.push_back(f1 - f0);
        cpu_acc += res.Value().cpu_ms;
        gpu_acc += res.Value().gpu_ms;
        ++n;
        rt += 16;  // 模拟每帧前进 16ms（静态场景插值无影响）
    }

    const double p95 = Percentile95(frame_ms);
    const double fps_p95 = (p95 > 0.0) ? (1000.0 / p95) : 0.0;

    const RenderStats s = r.LastStats();
    const double tex_mb = static_cast<double>(s.texture_memory_bytes) / (1024.0 * 1024.0);
    const double mesh_mb = static_cast<double>(s.mesh_memory_bytes) / (1024.0 * 1024.0);
    const double denom = (n > 0) ? static_cast<double>(n) : 1.0;

    // 组装文件内容（vsnprintf + fwrite，规避 printf( 红线）
    char buf[256];
    std::string out;
    auto append = [&](const char* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        out += buf;
    };
    append("draw_calls=%u\n", (unsigned)s.draw_calls);
    append("triangles=%u\n", (unsigned)s.triangles);
    append("texture_mem_mb=%.2f\n", tex_mb);
    append("mesh_mem_mb=%.2f\n", mesh_mb);
    append("cpu_ms=%.4f\n", cpu_acc / denom);
    append("gpu_ms=%.4f\n", gpu_acc / denom);
    append("fps_p95=%.2f\n", fps_p95);
    append("shader_switches=%u\n", (unsigned)s.shader_switches);

    const std::string fname = "bench/render_" + a.quality + ".txt";
    std::FILE* fp = std::fopen(fname.c_str(), "w");
    if (fp) {
        std::fwrite(out.data(), 1, out.size(), fp);
        std::fclose(fp);
    } else {
        tprint::Error("WARN: cannot write ");
        tprint::Error(fname.c_str());
        tprint::Error("\n");
    }

    // stdout 同步输出（验收脚本也会解析 bench 文件）
    tprint::Line(out.c_str());
    tprint::LineFmt("BENCH done frames=%d p95_ms=%.3f fps_p95=%.2f\n", n, p95, fps_p95);
    return 0;
}
