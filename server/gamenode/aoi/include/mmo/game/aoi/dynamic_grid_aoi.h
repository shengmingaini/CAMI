#pragma once

/// TASK-014 · DynamicGridAoi 具体实现（§7 / §15）。
///
/// 算法：稀疏格子哈希（unordered_map<CellKey, vector<EntityId>>），查询只扫描
/// ceil(view_radius/cell_size) 邻域格子 + 距离过滤，禁止全 Scene O(N) 扫描（§21）。
/// 可见集**不缓存**（每次 QueryVisible / Move / Broadcast 由位置实时重算），保证与暴力
/// O(N²) 参考实现 100% 一致（§20 验收 #2），且单实体 AOI 内存 < 128B（§22）。

#include <cstdint>
#include <cstddef>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "mmo/game/aoi/aoi.h"

namespace mmo::game::aoi {

/// 内存追踪分配器：统计 AOI 容器堆占用，供 benchmark 输出 mem_bytes_per_entity（§22 / §24）。
/// 全局计数器，bench 每档前 ResetAoiHeapTracking() 后测量（§18）。
inline std::uint64_t g_aoi_heap_bytes = 0;
template <class T>
struct TrackingAlloc {
    using value_type = T;
    TrackingAlloc() = default;
    template <class U>
    TrackingAlloc(const TrackingAlloc<U>&) noexcept {}
    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
        g_aoi_heap_bytes += bytes;
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T* p, std::size_t n) noexcept {
        g_aoi_heap_bytes -= n * sizeof(T);
        std::allocator<T>{}.deallocate(p, n);
    }
    template <class U>
    bool operator==(const TrackingAlloc<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const TrackingAlloc<U>&) const noexcept { return false; }
};
inline void ResetAoiHeapTracking() noexcept { g_aoi_heap_bytes = 0; }
inline std::uint64_t AoiHeapBytes() noexcept { return g_aoi_heap_bytes; }

/// 格子坐标（世界坐标 / cell_size 向下取整，负坐标用 floor）。
struct CellKey {
    std::int32_t x = 0;
    std::int32_t y = 0;
};
inline bool operator==(const CellKey& a, const CellKey& b) noexcept {
    return a.x == b.x && a.y == b.y;
}
struct CellKeyHash {
    std::size_t operator()(const CellKey& k) const noexcept {
        // 32-bit 交错哈希（低 16 位交错），减少相邻格子聚集到同一桶。
        const std::uint32_t a = static_cast<std::uint32_t>(k.x);
        const std::uint32_t b = static_cast<std::uint32_t>(k.y);
        std::uint32_t h = a ^ (b + 0x9E3779B9u + (a << 6) + (a >> 2));
        h ^= h >> 16;
        h *= 0x85EBCA6Bu;
        h ^= h >> 13;
        return static_cast<std::size_t>(h);
    }
};

/// 广播投递回调（扩展点，非冻结接口）：from 把 payload 发给 to。
using BroadcastSink = std::function<void(EntityId from, EntityId to,
                                         std::span<const std::uint8_t> payload)>;
/// 视野变化通知回调（§8 事件：EntityEnteredView / EntityLeftView 的扩展点）。
using NotifySink = std::function<void(EntityId watcher, EntityId seen, bool entered)>;

/// Dynamic Grid AOI 具体实现。
class DynamicGridAoi : public IAoi {
public:
    explicit DynamicGridAoi(AoiConfig config = {});
    ~DynamicGridAoi() override = default;

    // 扩展点（不影响冻结接口）：注入广播/通知回调，供集成测试观察投递（§17）。
    void SetBroadcastSink(BroadcastSink sink) { broadcast_sink_ = std::move(sink); }
    void SetNotifySink(NotifySink sink) { notify_sink_ = std::move(sink); }

    core::Result<void> Enter(EntityId id, const Position& pos) override;
    core::Result<void> Leave(EntityId id) override;
    core::Result<MoveResult> Move(EntityId id, const Position& to) override;
    core::Result<void> QueryVisible(EntityId id, std::vector<EntityId>& out) const override;
    core::Result<std::size_t> Broadcast(EntityId id,
                                        std::span<const std::uint8_t> payload) override;
    AoiStats Stats() const noexcept override;

    // 诊断：当前格子数 / 实体数（stats 之外）。
    std::size_t CellCount() const noexcept { return cells_.size(); }

private:
    CellKey CellOf(const Position& p) const noexcept;
    /// 实时计算 id（位于 pos）的可见集，写入 out（去重，不含自身）。
    void ComputeVisible(EntityId id, const Position& pos, std::vector<EntityId>& out) const;
    /// 3D 欧氏距离（米）。
    static float Dist3(const Position& a, const Position& b) noexcept;
    bool IsValidCoord(const Position& p) const noexcept;

    AoiConfig config_;

    struct EntityRecord {
        float x = 0, y = 0, z = 0;
        CellKey cell{};
    };
    using EntityMap = std::unordered_map<EntityId, EntityRecord, std::hash<EntityId>,
                                         std::equal_to<EntityId>,
                                         TrackingAlloc<std::pair<const EntityId, EntityRecord>>>;
    using CellVec = std::vector<EntityId, TrackingAlloc<EntityId>>;
    using CellMap = std::unordered_map<CellKey, CellVec, CellKeyHash, std::equal_to<CellKey>,
                                       TrackingAlloc<std::pair<const CellKey, CellVec>>>;

    EntityMap entities_;
    CellMap cells_;

    // 广播复用缓冲区：每次 Broadcast 只 resize 一次，禁止每目标分配（§15.5 / §21）。
    std::vector<std::uint8_t, TrackingAlloc<std::uint8_t>> send_buf_;

    BroadcastSink broadcast_sink_;
    NotifySink notify_sink_;

    // 统计
    mutable std::uint64_t last_query_ns_{0};
    mutable std::uint64_t last_broadcast_ns_{0};
    mutable std::size_t query_count_{0};
};

}  // namespace mmo::game::aoi
