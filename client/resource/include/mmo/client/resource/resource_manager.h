#pragma once

/// TASK-036 · 资源管理器（ResourceManager）公开接口（签名冻结）。
///
/// 职责：异步加载 / 引用计数 / LRU 回收 / 三档独立预算（纹理 / 网格 / 音频）/
///       画质热切换。headless 环境下用真实字节记账（std::vector<uint8_t> 实际分配），
///       VRAM 为按真实资源维度推导的投影记账（GPU/D3D11 后端为桩，见 PERFORMANCE.md）。
///
/// 线程模型：加载在 Worker 线程池（构造时按 CPU 核数起 2~4 线程），主线程仅在
///           LoadAsync / Unload / SetQuality / Stats 调用点（安全点）做合并与记账，
///           不阻塞。Tick 热路径内禁止同步 IO / 大块分配。

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mmo/client/resource/quality_preset.h"
#include "mmo/core/error/result.h"

namespace mmo::client::resource {

/// 资源类型（冻结）。
enum class ResourceType : std::uint8_t {
    Texture = 0,
    Mesh = 1,
    Audio = 2,
};

using ResourceId = std::uint64_t;

/// 资源句柄：持有引用计数 id。id==0 表示无效。
struct ResourceHandle {
    ResourceId id = 0;
    bool IsValid() const noexcept { return id != 0; }
};

/// 资源统计快照（真实数字，统计面板 / benchmark 直接消费）。
/// 前 7 个字段为规格冻结字段；其后为派生 / 扩展字段（真实可测，不改动既有字段语义）。
struct ResourceStats {
    std::size_t   ram_bytes = 0;       // 真实 CPU 侧字节占用（实际分配之和）
    std::size_t   vram_bytes = 0;      // 投影 GPU 字节占用（纹理+网格，真实维度推导）
    std::size_t   texture_bytes = 0;   // 纹理占用
    std::size_t   mesh_bytes = 0;      // 网格占用
    std::size_t   audio_bytes = 0;     // 音频占用
    std::uint32_t handle_count = 0;    // 当前句柄总数（含 pending）
    std::uint32_t lru_evictions = 0;   // 累计 LRU 回收次数
    // ---- 扩展（真实派生）----
    double   ram_mb = 0.0;             // ram_bytes / 1MB
    double   vram_mb = 0.0;            // vram_bytes / 1MB
    std::uint32_t texture_count = 0;   // 已提交纹理数
    std::uint32_t mesh_count = 0;      // 已提交网格数
    std::uint32_t audio_count = 0;     // 已提交音频数
    std::uint32_t visible_entities = 0;// 经视距/同屏上限裁剪后的可见实体数
    std::uint32_t draw_calls = 0;      // 由可见实体数派生（<= max_visible_entities）
    double   cache_hit_rate = 0.0;     // 命中 / 总请求
    double   load_ms_p95 = 0.0;        // 真实加载耗时样本 P95（毫秒）
    std::uint64_t total_requests = 0;  // 累计 LoadAsync 调用
    std::uint64_t cache_hits = 0;      // 累计缓存命中
};

class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();

    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    /// 用某档预设初始化（设定三档预算与 LOD 参数）。重复调用即重置预算。
    core::Result<void> Init(const QualityPreset& preset);

    /// 异步加载：立即返回句柄（pending 状态），Worker 后台完成真实分配后提交。
    /// 同 uri 复用并 +1 引用；返回 Result<ResourceHandle>。
    core::Result<ResourceHandle> LoadAsync(std::string_view uri, ResourceType type);

    /// 释放一个引用；归零后成为 LRU 候选（不立即删除，等预算压力回收）。
    core::Result<void> Unload(ResourceHandle handle);

    /// 热切换画质：按新预算回收超预算资源（先纹理后网格），重建 LOD 距离。
    core::Result<void> SetQuality(QualityLevel level);

    /// 取统计快照（主线程安全点调用，不阻塞）。
    ResourceStats Stats() const noexcept;

private:
    struct Entry {
        ResourceId    id = 0;
        ResourceType type = ResourceType::Texture;
        std::string  uri;
        std::size_t  bytes = 0;        // 已提交后的真实 RAM 字节
        std::size_t  vram_bytes = 0;   // 已提交后的投影 VRAM 字节
        std::uint32_t ref_count = 0;
        std::uint64_t last_used = 0;   // LRU 时钟（越大越新）
        bool         committed = false; // false = Worker 仍在加载
        std::vector<std::uint8_t> data;// 真实分配（RAM 记账来源）
    };

    void UpdateBudgets() noexcept;
    bool EvictOne(ResourceType cat);
    void EvictToBudget() noexcept;
    std::size_t ComputePayload(ResourceType type, std::string_view uri) const;
    std::size_t ComputeVram(ResourceType type, std::string_view uri) const;
    double ComputeP95() const;
    void RecordLoadSample(double ms);
    void AddUsed(const Entry& e) noexcept;
    void SubUsed(const Entry& e) noexcept;
    void WorkerLoop();
    void StopWorkers();

    mutable std::mutex mutex_;
    QualityPreset preset_{};
    bool inited_ = false;
    std::uint64_t next_id_ = 1;
    std::uint64_t tick_counter_ = 0;

    std::unordered_map<ResourceId, Entry> entries_;
    std::unordered_map<std::string, ResourceId> uri_index_;

    // 三档独立预算（字节）
    std::size_t tex_budget_ = 0;
    std::size_t mesh_budget_ = 0;
    std::size_t audio_budget_ = 0;
    // 已用（已提交）
    std::size_t tex_used_ = 0;
    std::size_t mesh_used_ = 0;
    std::size_t audio_used_ = 0;
    std::uint32_t tex_count_ = 0;
    std::uint32_t mesh_count_ = 0;
    std::uint32_t audio_count_ = 0;

    std::uint64_t total_requests_ = 0;
    std::uint64_t cache_hits_ = 0;
    std::uint64_t lru_evictions_ = 0;

    static constexpr std::size_t kMaxSamples = 8192;
    std::vector<double> load_samples_;
    std::size_t sample_pos_ = 0;

    // Worker 线程池
    std::vector<std::thread> workers_;
    std::mutex q_mutex_;
    std::condition_variable q_cv_;
    std::queue<ResourceId> job_queue_;
    std::atomic<bool> stop_{false};
};

}  // namespace mmo::client::resource
