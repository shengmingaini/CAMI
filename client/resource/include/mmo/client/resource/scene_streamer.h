#pragma once

/// TASK-036 · 场景流式加载（SceneStreamer）公开接口（签名冻结）。
///
/// 把世界按 chunk_size 切成网格；Update 根据玩家位置计算当前 chunk + 半径内邻居，
/// 异步「加载」进入半径的 chunk，对离开超过 unload_delay_seconds 的 chunk 延迟卸载
/// （迟滞 hysteresis，防来回穿越边界触发加载风暴）。每帧限流最多处理
/// max_pending_loads 个加载完成回调。headless 下以帧时间模拟异步加载延迟。

#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "mmo/client/types.h"
#include "mmo/core/error/result.h"

namespace mmo::client::resource {

/// 单帧上下文（自定义；主线程每帧传入）。
struct FrameContext {
    double      dt_ms = 16.0;        // 本帧耗时（毫秒）
    std::uint32_t frame_index = 0;  // 帧序号（单调递增）
};

/// 流式加载统计快照（真实计数）。
struct StreamingStats {
    std::uint32_t loaded_chunks = 0;   // 当前已加载 chunk 数
    std::uint32_t pending_loads = 0;   // 正在异步加载的 chunk 数
    double        last_load_ms = 0.0;  // 最近一次 chunk 加载耗时（毫秒）
    std::uint32_t unload_count = 0;    // 累计已卸载 chunk 数
    // ---- 扩展（真实派生）----
    std::uint32_t requested_loads = 0;     // 累计发起的加载请求数（抗风暴验证）
    std::uint32_t unloaded_chunks = 0;     // 累计完成卸载的 chunk 数
    std::uint32_t max_loaded_ever = 0;     // 历史同时常驻峰值
};

class SceneStreamer {
public:
    struct Config {
        float     chunk_size = 128.0f;        // 单 chunk 边长（米）
        std::uint32_t load_radius = 2;        // 当前 + 周围 N 圈
        std::uint32_t unload_delay_seconds = 5; // 离开后延迟卸载（迟滞）
        std::uint32_t max_pending_loads = 4;  // 每帧最多完成 N 个加载回调
    };

    SceneStreamer();
    explicit SceneStreamer(const Config& cfg);
    ~SceneStreamer() = default;

    void Configure(const Config& cfg) noexcept { cfg_ = cfg; }

    /// 主线程每帧调用：根据玩家位置推进分块加载/卸载状态机。
    core::Result<void> Update(const Vec3& player_pos, const FrameContext& frame);

    /// 立刻卸载所有已加载 chunk（切场景 / 退出时）。
    core::Result<void> ForceUnloadAll();

    StreamingStats Stats() const noexcept;

private:
    enum class ChunkState { Unloaded, Pending, Loaded, Unloading };

    struct ChunkStateData {
        int cx = 0;
        int cz = 0;
        ChunkState state = ChunkState::Unloaded;
        double load_progress_ms = 0.0;  // 已累计加载时间
        double load_needed_ms = 0.0;    // 需要的加载时长（确定性推导）
        double unload_timer = 0.0;      // 卸载倒计时（秒）
    };

    static std::int64_t Key(int cx, int cz) noexcept;

    Config cfg_{};
    mutable std::mutex mutex_;
    std::unordered_map<std::int64_t, ChunkStateData> chunks_;
    StreamingStats stats_{};
};

}  // namespace mmo::client::resource
