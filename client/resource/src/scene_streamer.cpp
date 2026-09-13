// client/resource/src/scene_streamer.cpp —— 分块流式加载（headless 时间模拟）。
//
// 迟滞（hysteresis）：进入半径即开始加载；离开半径先进入 Unloading 倒计时，倒计时内
// 若重新进入半径则取消卸载（保持 Loaded），避免来回穿越边界触发加载风暴。
// 每帧限流：新发起加载与完成回调均受 max_pending_loads 约束。

#include "mmo/client/resource/scene_streamer.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace mmo::client::resource {

SceneStreamer::SceneStreamer() = default;

SceneStreamer::SceneStreamer(const Config& cfg) : cfg_(cfg) {}

std::int64_t SceneStreamer::Key(int cx, int cz) noexcept {
    // 用 32 位偏移避免负数碰撞。
    const std::int64_t ox = static_cast<std::int64_t>(cx) + 0x80000000LL;
    const std::int64_t oz = static_cast<std::int64_t>(cz) + 0x80000000LL;
    return (ox << 32) | (oz & 0xFFFFFFFFLL);
}

core::Result<void> SceneStreamer::Update(const Vec3& player_pos, const FrameContext& frame) {
    std::lock_guard<std::mutex> lk(mutex_);
    const float cs = cfg_.chunk_size > 0.0f ? cfg_.chunk_size : 128.0f;
    const int r = static_cast<int>(cfg_.load_radius);
    const int cx = static_cast<int>(std::floor(static_cast<double>(player_pos.x) / static_cast<double>(cs)));
    const int cz = static_cast<int>(std::floor(static_cast<double>(player_pos.z) / static_cast<double>(cs)));
    const double dt = frame.dt_ms > 0.0 ? frame.dt_ms : 16.0;

    std::uint32_t newLoadsThisFrame = 0;
    std::uint32_t completionsThisFrame = 0;

    // ---- Step A：推进既有 chunk（pending->loaded 完成；unloading 倒计时） ----
    for (auto& kv : chunks_) {
        ChunkStateData& c = kv.second;
        if (c.state == ChunkState::Pending) {
            c.load_progress_ms += dt;
            if (c.load_progress_ms >= c.load_needed_ms) {
                if (completionsThisFrame < cfg_.max_pending_loads) {
                    c.state = ChunkState::Loaded;
                    stats_.loaded_chunks++;
                    stats_.last_load_ms = c.load_needed_ms;
                    completionsThisFrame++;
                    if (stats_.loaded_chunks > stats_.max_loaded_ever)
                        stats_.max_loaded_ever = stats_.loaded_chunks;
                }
                // 超帧预算则留到下一帧完成。
            }
        } else if (c.state == ChunkState::Unloading) {
            c.unload_timer -= dt / 1000.0;
            if (c.unload_timer <= 0.0) {
                c.state = ChunkState::Unloaded;
                if (stats_.loaded_chunks > 0) stats_.loaded_chunks--;
                stats_.unload_count++;
                stats_.unloaded_chunks++;
            }
        }
    }

    // ---- Step B：离开半径的 Loaded chunk -> Unloading（迟滞开始） ----
    for (auto& kv : chunks_) {
        ChunkStateData& c = kv.second;
        if (c.state != ChunkState::Loaded) continue;
        const int ddx = c.cx - cx;
        const int ddz = c.cz - cz;
        const int cheb = std::max(std::abs(ddx), std::abs(ddz));
        if (cheb > r) {
            c.state = ChunkState::Unloading;
            c.unload_timer = static_cast<double>(cfg_.unload_delay_seconds);
        }
    }

    // ---- Step C：半径内邻居 -> 缺失则发起加载（限流）；Unloading 重新进入则取消 ----
    for (int dz = -r; dz <= r; ++dz) {
        for (int dx = -r; dx <= r; ++dx) {
            const int nx = cx + dx;
            const int nz = cz + dz;
            const std::int64_t k = Key(nx, nz);
            auto it = chunks_.find(k);
            if (it == chunks_.end()) {
                if (newLoadsThisFrame < cfg_.max_pending_loads) {
                    ChunkStateData c;
                    c.cx = nx;
                    c.cz = nz;
                    c.state = ChunkState::Pending;
                    c.load_progress_ms = 0.0;
                    // 确定性加载时长 40~120ms。
                    const int h = std::abs(nx * 31 + nz * 17);
                    c.load_needed_ms = 40.0 + static_cast<double>(h % 80);
                    chunks_.emplace(k, c);
                    stats_.requested_loads++;
                    newLoadsThisFrame++;
                }
            } else {
                ChunkStateData& c = it->second;
                if (c.state == ChunkState::Unloading) {
                    // 迟滞取消：重新进入半径，保留已加载状态。
                    c.state = ChunkState::Loaded;
                    stats_.loaded_chunks++;
                }
                // Pending / Loaded 无需动作。
            }
        }
    }

    (void)newLoadsThisFrame;
    return core::Result<void>::Ok();
}

core::Result<void> SceneStreamer::ForceUnloadAll() {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& kv : chunks_) {
        ChunkStateData& c = kv.second;
        if (c.state == ChunkState::Loaded || c.state == ChunkState::Unloading) {
            stats_.unload_count++;
        }
        c.state = ChunkState::Unloaded;
    }
    stats_.loaded_chunks = 0;
    return core::Result<void>::Ok();
}

StreamingStats SceneStreamer::Stats() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    StreamingStats s = stats_;
    std::uint32_t loaded = 0;
    std::uint32_t pend = 0;
    for (const auto& kv : chunks_) {
        if (kv.second.state == ChunkState::Loaded) loaded++;
        else if (kv.second.state == ChunkState::Pending) pend++;
    }
    s.loaded_chunks = loaded;
    s.pending_loads = pend;
    return s;
}

}  // namespace mmo::client::resource
