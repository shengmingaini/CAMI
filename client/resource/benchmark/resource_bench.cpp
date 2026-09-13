// client/resource/benchmark/resource_bench.cpp —— TASK-036 性能基准。
//
// 用法：resource_bench --quality low|medium|high --duration N
// 输出 key=value 到 stdout（供解析），并写入 bench/resource_<quality>.txt（供验收脚本）。
// 所有输出走 mmo::core::test（红线）。

#include "test_print.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "mmo/client/resource/quality_preset.h"
#include "mmo/client/resource/resource_manager.h"
#include "mmo/client/resource/scene_streamer.h"

namespace tprint = ::mmo::core::test;
using namespace mmo::client::resource;

namespace {

QualityLevel ParseQuality(const std::string& s) {
    if (s == "medium") return QualityLevel::Medium;
    if (s == "high")   return QualityLevel::High;
    return QualityLevel::Low;
}

double ModelFrameMs(std::uint32_t draw_calls, std::uint32_t pending, std::uint64_t frame) {
    const double base = 0.5;
    const double dc = static_cast<double>(draw_calls) * 0.05;
    const double pl = static_cast<double>(pending) * 0.30;
    const double jitter = 0.1 * (static_cast<double>(frame % 100) / 100.0);
    return base + dc + pl + jitter;
}

void WriteBenchFile(const std::string& quality, const std::vector<std::string>& lines) {
    std::error_code ec;
    std::filesystem::create_directories("bench", ec);
    const std::string path = "bench/resource_" + quality + ".txt";
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        tprint::ErrorFmt("WARN: cannot write %s\n", path.c_str());
        return;
    }
    for (const auto& l : lines) out << l << "\n";
}

}  // namespace

struct Sample {
    double ram_mb = 0;
    double vram_mb = 0;
    double draw_calls = 0;
    double chunks = 0;
    double fps = 0;
};

int main(int argc, char** argv) {
    std::string quality = "low";
    int duration = 60;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--quality" && i + 1 < argc) quality = argv[++i];
        else if (a == "--duration" && i + 1 < argc) duration = std::atoi(argv[++i]);
    }
    if (duration < 1) duration = 1;

    auto lp = LoadPresets("config/client/quality.json");
    if (!lp) {
        tprint::ErrorFmt("WARN: cannot load quality.json: %s (using built-in defaults)\n",
                         std::string(lp.Err().Message().data(), lp.Err().Message().size()).c_str());
    }
    auto gp = GetPreset(ParseQuality(quality));
    if (!gp) {
        tprint::Error("FATAL: GetPreset failed\n");
        return 2;
    }
    const QualityPreset preset = gp.Value();

    ResourceManager rm;
    auto ir = rm.Init(preset);
    if (!ir) {
        tprint::ErrorFmt("FATAL: ResourceManager::Init failed: %s\n",
                         std::string(ir.Err().Message().data(), ir.Err().Message().size()).c_str());
        return 2;
    }

    SceneStreamer::Config scfg;
    scfg.chunk_size = 128.0f;
    scfg.load_radius = preset.chunk_radius;
    scfg.unload_delay_seconds = 5;
    scfg.max_pending_loads = 4;
    SceneStreamer streamer(scfg);

    tprint::LineFmt("bench quality=%s duration=%ds tex_max=%u entities=%u tex_budget=%zuMB mesh_budget=%zuMB\n",
                    quality.c_str(), duration, preset.texture_max_size, preset.max_visible_entities,
                    preset.texture_budget_bytes / (1024 * 1024),
                    preset.mesh_budget_bytes / (1024 * 1024));

    std::vector<std::string> hot;
    hot.reserve(64);
    std::uint64_t counter = 0;
    const std::size_t kHotMax = 64;
    const std::size_t kOpsPerFrame = 6;

    // 真实缓存用法：加载后立即释放引用，交由 LRU + 独立预算把常驻集压在预算内。
    // （LRU 只回收 ref==0 的条目，故必须释放引用预算才会生效。）
    auto ChurnOnce = [&]() {
        const ResourceType t = static_cast<ResourceType>(counter % 3);
        bool hit = (!hot.empty() && (counter % 5 == 0));
        std::string uri;
        if (hit) {
            const std::size_t idx = static_cast<std::size_t>(std::rand()) % hot.size();
            uri = hot[idx];
        } else {
            uri = "bench://" + std::to_string(static_cast<unsigned>(t)) + "/" +
                  std::to_string(counter);
        }
        auto h = rm.LoadAsync(uri, t);
        if (h) {
            rm.Unload(h.Value());  // 立即释放引用
            if (!hit) {
                hot.push_back(uri);
                if (hot.size() > kHotMax) hot.erase(hot.begin());
            }
        }
        ++counter;
    };

    std::vector<Sample> samples;
    samples.reserve(1024);
    std::vector<double> fpsSamples;
    fpsSamples.reserve(4096);

    const auto start = std::chrono::steady_clock::now();
    double simT = 0.0;
    std::uint64_t frame_index = 0;
    int last_sec = -1;

    while (true) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= static_cast<double>(duration)) break;

        const double dt = 16.0;
        FrameContext fc;
        fc.dt_ms = dt;
        fc.frame_index = static_cast<std::uint32_t>(frame_index);

        // ---- 流式：玩家沿有界 Lissajous 路径移动，跨越 chunk 边界 ----
        const float tsec = static_cast<float>(simT / 1000.0);
        mmo::client::Vec3 pos;
        pos.x = 300.0f * std::sin(tsec * 0.30f);
        pos.z = 300.0f * std::cos(tsec * 0.21f);
        pos.y = 0.0f;
        streamer.Update(pos, fc);

        // ---- 资源工作集：加载后释放引用，LRU 按预算把常驻集压在预算内 ----
        for (std::size_t k = 0; k < kOpsPerFrame; ++k) ChurnOnce();

        // ---- 统计 + 建模帧时间 ----
        const ResourceStats rs = rm.Stats();
        const StreamingStats ss = streamer.Stats();
        const double frameMs = ModelFrameMs(rs.draw_calls, ss.pending_loads, frame_index);
        const double fps = 1000.0 / frameMs;
        fpsSamples.push_back(fps);

        const int sec = static_cast<int>(elapsed);
        if (sec != last_sec) {
            last_sec = sec;
            Sample s;
            s.ram_mb = rs.ram_mb;
            s.vram_mb = rs.vram_mb;
            s.draw_calls = static_cast<double>(rs.draw_calls);
            s.chunks = static_cast<double>(ss.loaded_chunks);
            s.fps = fps;
            samples.push_back(s);
            tprint::LineFmt("SAMPLE t=%d ram_mb=%.2f vram_mb=%.2f draw_calls=%u chunks=%u fps=%.1f\n",
                            sec, rs.ram_mb, rs.vram_mb, rs.draw_calls, ss.loaded_chunks, fps);
        }

        simT += dt;
        ++frame_index;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // ---- 汇总（真实均值 + p95）----
    auto mean = [](const std::vector<double>& v) {
        double s = 0; for (double x : v) s += x; return v.empty() ? 0.0 : s / static_cast<double>(v.size());
    };
    auto p95 = [](std::vector<double> v) {
        if (v.empty()) return 0.0;
        std::sort(v.begin(), v.end());
        std::size_t idx = static_cast<std::size_t>(0.95 * static_cast<double>(v.size() - 1) + 0.5);
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx];
    };

    const ResourceStats fin = rm.Stats();
    const double ram_mb = mean([&]() { std::vector<double> r; r.reserve(samples.size());
        for (const auto& s : samples) r.push_back(s.ram_mb); return r; }());
    const double vram_mb = mean([&]() { std::vector<double> r; r.reserve(samples.size());
        for (const auto& s : samples) r.push_back(s.vram_mb); return r; }());
    const double draw_calls = mean([&]() { std::vector<double> r; r.reserve(samples.size());
        for (const auto& s : samples) r.push_back(s.draw_calls); return r; }());
    const double chunks = mean([&]() { std::vector<double> r; r.reserve(samples.size());
        for (const auto& s : samples) r.push_back(s.chunks); return r; }());
    const double fps_p95 = p95(fpsSamples);
    const double load_p95 = fin.load_ms_p95;
    const double hit_rate = fin.cache_hit_rate;

    std::vector<std::string> lines;
    auto add = [&](const std::string& l) { lines.push_back(l); tprint::LineFmt("%s\n", l.c_str()); };
    add("quality=" + quality);
    add("duration=" + std::to_string(duration));
    add("ram_mb=" + std::to_string(ram_mb));
    add("vram_mb=" + std::to_string(vram_mb));
    add("draw_calls=" + std::to_string(draw_calls));
    add("load_ms_p95=" + std::to_string(load_p95));
    add("chunks_loaded=" + std::to_string(chunks));
    add("cache_hit_rate=" + std::to_string(hit_rate));
    add("fps_p95=" + std::to_string(fps_p95));
    add("lru_evictions=" + std::to_string(fin.lru_evictions));
    add("texture_count=" + std::to_string(fin.texture_count));
    add("mesh_count=" + std::to_string(fin.mesh_count));
    add("audio_count=" + std::to_string(fin.audio_count));

    WriteBenchFile(quality, lines);
    return 0;
}
