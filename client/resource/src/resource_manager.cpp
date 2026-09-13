// client/resource/src/resource_manager.cpp —— 资源管理器实现（headless 真实字节记账）。
//
// 不输出任何 stdout/stderr（红线）。错误一律走 core::Result / core::Error。

#include "mmo/client/resource/resource_manager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

namespace mmo::client::resource {

namespace {

// 确定性 FNV-1a 64，用于把 uri 映射为稳定尺寸（不依赖 std::hash 平台差异）。
std::uint64_t Fn64(std::string_view s) noexcept {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : s) {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= 1099511628211ULL;
    }
    return h;
}

// 音频缓存独立预算（非画质预设字段，纯内部缓存上限；其余两类来自 preset）。
constexpr std::size_t kAudioBudgetBytes = 64ULL * 1024 * 1024;

}  // namespace

ResourceManager::ResourceManager() {
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned n = std::max<unsigned>(2u, std::min<unsigned>(4u, hw == 0 ? 2u : hw));
    for (unsigned i = 0; i < n; ++i) {
        workers_.emplace_back(&ResourceManager::WorkerLoop, this);
    }
}

ResourceManager::~ResourceManager() {
    StopWorkers();
}

void ResourceManager::StopWorkers() {
    stop_.store(true);
    {
        std::unique_lock<std::mutex> lk(q_mutex_);
        (void)lk;
        q_cv_.notify_all();
    }
    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();
}

void ResourceManager::UpdateBudgets() noexcept {
    tex_budget_ = preset_.texture_budget_bytes;
    mesh_budget_ = preset_.mesh_budget_bytes;
    audio_budget_ = kAudioBudgetBytes;
}

core::Result<void> ResourceManager::Init(const QualityPreset& preset) {
    std::lock_guard<std::mutex> lk(mutex_);
    preset_ = preset;
    inited_ = true;
    UpdateBudgets();
    return core::Result<void>::Ok();
}

std::size_t ResourceManager::ComputePayload(ResourceType type, std::string_view uri) const {
    switch (type) {
        case ResourceType::Texture: {
            const std::size_t texel = static_cast<std::size_t>(preset_.texture_max_size);
            return texel * texel * 4u;  // RGBA8 解码尺寸
        }
        case ResourceType::Mesh: {
            const std::uint64_t h = Fn64(uri);
            const std::size_t verts = 2000u + static_cast<std::size_t>(h % 8000u);
            return verts * 32u;  // pos+normal+uv+...+index 近似
        }
        case ResourceType::Audio: {
            const std::uint64_t h = Fn64(uri);
            const std::size_t samples = 50000u + static_cast<std::size_t>(h % 150000u);
            return samples * 2u;  // 16-bit PCM
        }
    }
    return 0;
}

std::size_t ResourceManager::ComputeVram(ResourceType type, std::string_view uri) const {
    if (type == ResourceType::Audio) return 0;
    return ComputePayload(type, uri);
}

void ResourceManager::AddUsed(const Entry& e) noexcept {
    switch (e.type) {
        case ResourceType::Texture: tex_used_ += e.bytes; tex_count_++; break;
        case ResourceType::Mesh:    mesh_used_ += e.bytes; mesh_count_++; break;
        case ResourceType::Audio:   audio_used_ += e.bytes; audio_count_++; break;
    }
}

void ResourceManager::SubUsed(const Entry& e) noexcept {
    switch (e.type) {
        case ResourceType::Texture: tex_used_ -= e.bytes; if (tex_count_) tex_count_--; break;
        case ResourceType::Mesh:    mesh_used_ -= e.bytes; if (mesh_count_) mesh_count_--; break;
        case ResourceType::Audio:   audio_used_ -= e.bytes; if (audio_count_) audio_count_--; break;
    }
}

void ResourceManager::RecordLoadSample(double ms) {
    if (load_samples_.size() < kMaxSamples) {
        load_samples_.push_back(ms);
    } else {
        load_samples_[sample_pos_] = ms;
        sample_pos_ = (sample_pos_ + 1) % kMaxSamples;
    }
}

double ResourceManager::ComputeP95() const {
    if (load_samples_.empty()) return 0.0;
    std::vector<double> tmp = load_samples_;
    const std::size_t n = tmp.size();
    std::size_t idx = static_cast<std::size_t>(0.95 * static_cast<double>(n - 1) + 0.5);
    if (idx >= n) idx = n - 1;
    std::nth_element(tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(idx), tmp.end());
    return tmp[idx];
}

core::Result<ResourceHandle> ResourceManager::LoadAsync(std::string_view uri, ResourceType type) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!inited_) {
        return core::Result<ResourceHandle>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "ResourceManager not initialized", core::domain::kData));
    }
    ++total_requests_;
    const std::string key(uri.data(), uri.size());

    auto it = uri_index_.find(key);
    if (it != uri_index_.end()) {
        auto eit = entries_.find(it->second);
        if (eit != entries_.end()) {
            ++cache_hits_;
            eit->second.ref_count++;
            eit->second.last_used = ++tick_counter_;
            return core::Result<ResourceHandle>::Ok(ResourceHandle{it->second});
        }
    }

    const ResourceId id = next_id_++;
    Entry e;
    e.id = id;
    e.type = type;
    e.uri = key;
    e.ref_count = 1;
    e.last_used = ++tick_counter_;
    e.committed = false;
    entries_.emplace(id, std::move(e));
    uri_index_.emplace(std::move(key), id);

    {
        std::unique_lock<std::mutex> qlk(q_mutex_);
        job_queue_.push(id);
        q_cv_.notify_one();
    }
    return core::Result<ResourceHandle>::Ok(ResourceHandle{id});
}

core::Result<void> ResourceManager::Unload(ResourceHandle handle) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (handle.id == 0) return core::Result<void>::Ok();
    auto it = entries_.find(handle.id);
    if (it == entries_.end()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "resource handle not found", core::domain::kData));
    }
    Entry& e = it->second;
    if (e.ref_count == 0) {
        // 已完全释放，保持幂等（不再下溢）。
        return core::Result<void>::Ok();
    }
    --e.ref_count;
    // 归零后成为 LRU 候选：刷新 last_used 使其成为「最新释放」，LRU 优先回收更早释放者。
    e.last_used = ++tick_counter_;
    // 释放后立刻尝试按预算回收（避免已释放资源长期占用超预算内存）。
    EvictToBudget();
    return core::Result<void>::Ok();
}

bool ResourceManager::EvictOne(ResourceType cat) {
    ResourceId victim = 0;
    std::uint64_t oldest = ~static_cast<std::uint64_t>(0);
    for (const auto& kv : entries_) {
        const Entry& e = kv.second;
        if (e.type == cat && e.committed && e.ref_count == 0 && e.last_used < oldest) {
            oldest = e.last_used;
            victim = e.id;
        }
    }
    if (victim == 0) return false;
    auto it = entries_.find(victim);
    if (it == entries_.end()) return false;
    SubUsed(it->second);
    uri_index_.erase(it->second.uri);
    entries_.erase(it);
    ++lru_evictions_;
    return true;
}

void ResourceManager::EvictToBudget() noexcept {
    while (tex_used_ > tex_budget_) {
        if (!EvictOne(ResourceType::Texture)) break;
    }
    while (mesh_used_ > mesh_budget_) {
        if (!EvictOne(ResourceType::Mesh)) break;
    }
    while (audio_used_ > audio_budget_) {
        if (!EvictOne(ResourceType::Audio)) break;
    }
}

core::Result<void> ResourceManager::SetQuality(QualityLevel level) {
    auto p = GetPreset(level);
    if (!p) return core::Result<void>::Fail(std::move(p).Err());
    {
        std::lock_guard<std::mutex> lk(mutex_);
        preset_ = p.Value();
        UpdateBudgets();
        EvictToBudget();  // 先纹理后网格，按新（可能更紧）预算回收
    }
    return core::Result<void>::Ok();
}

ResourceStats ResourceManager::Stats() const noexcept {
    ResourceStats s;
    std::lock_guard<std::mutex> lk(mutex_);
    s.ram_bytes = tex_used_ + mesh_used_ + audio_used_;
    s.vram_bytes = tex_used_ + mesh_used_;  // 音频无 VRAM
    s.texture_bytes = tex_used_;
    s.mesh_bytes = mesh_used_;
    s.audio_bytes = audio_used_;
    s.handle_count = static_cast<std::uint32_t>(entries_.size());
    s.lru_evictions = static_cast<std::uint32_t>(lru_evictions_);
    s.texture_count = tex_count_;
    s.mesh_count = mesh_count_;
    s.audio_count = audio_count_;
    s.total_requests = total_requests_;
    s.cache_hits = cache_hits_;
    s.ram_mb = static_cast<double>(s.ram_bytes) / (1024.0 * 1024.0);
    s.vram_mb = static_cast<double>(s.vram_bytes) / (1024.0 * 1024.0);
    s.cache_hit_rate = (total_requests_ > 0)
        ? static_cast<double>(cache_hits_) / static_cast<double>(total_requests_)
        : 0.0;
    // draw_calls 由可见实体数派生，受 max_visible_entities 上限约束（保证 Low<300）。
    const std::uint32_t visible = std::min<std::uint32_t>(mesh_count_, preset_.max_visible_entities);
    s.visible_entities = visible;
    s.draw_calls = visible;
    s.load_ms_p95 = ComputeP95();
    return s;
}

void ResourceManager::WorkerLoop() {
    while (!stop_.load(std::memory_order_acquire)) {
        ResourceId id = 0;
        {
            std::unique_lock<std::mutex> lk(q_mutex_);
            q_cv_.wait(lk, [this]() { return stop_.load() || !job_queue_.empty(); });
            if (stop_.load() && job_queue_.empty()) return;
            if (job_queue_.empty()) continue;
            id = job_queue_.front();
            job_queue_.pop();
        }

        // 读取 entry 元数据（类型 / uri / 尺寸）与预设（只读，无需持锁）
        ResourceType type = ResourceType::Texture;
        std::string uri;
        std::size_t payload = 0;
        std::size_t vram = 0;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = entries_.find(id);
            if (it != entries_.end()) {
                type = it->second.type;
                uri = it->second.uri;
                found = true;
                payload = ComputePayload(type, uri);
                vram = ComputeVram(type, uri);
            }
        }
        if (!found) continue;

        const auto t0 = std::chrono::steady_clock::now();

        // 真实分配 + 确定性「解码」填充（headless 无 GPU，但 RAM 占用真实）。
        std::vector<std::uint8_t> data(payload);
        for (std::size_t i = 0; i < payload; ++i) {
            data[i] = static_cast<std::uint8_t>((i * 31u + (payload & 0xFFu)) & 0xFFu);
        }
        // 模拟磁盘/包体 IO 延迟（异步，不阻塞主线程）。
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        {
            std::lock_guard<std::mutex> lk(mutex_);
            auto it = entries_.find(id);
            if (it != entries_.end() && !it->second.committed) {
                Entry& e = it->second;
                e.committed = true;
                e.bytes = payload;
                e.vram_bytes = vram;
                e.data = std::move(data);
                AddUsed(e);
                RecordLoadSample(ms);
                EvictToBudget();
            }
        }
    }
}

}  // namespace mmo::client::resource
