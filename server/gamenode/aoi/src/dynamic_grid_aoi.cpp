// server/gamenode/aoi/src/dynamic_grid_aoi.cpp — TASK-014 §15
//
// Dynamic Grid AOI：稀疏格子哈希 + 邻域扫描 + 距离过滤。可见集实时由位置重算（不缓存），
// 保证与暴力 O(N²) 参考实现 100% 一致；查询局部化，禁止全 Scene 扫描。

#include "mmo/game/aoi/dynamic_grid_aoi.h"

#include <algorithm>
#include <cmath>

#include "mmo/core/error/error.h"
#include "mmo/core/time/clock.h"

namespace mmo::game::aoi {

namespace {
constexpr float kWorldHalf = 1'000'000.0f;  // 世界半边长（米）；越界坐标拒绝（§19）

inline bool IsFinite(float v) noexcept { return v == v; }  // NaN 自检（不依赖 <cmath> 行为）
}  // namespace

DynamicGridAoi::DynamicGridAoi(AoiConfig config) : config_(std::move(config)) {}

CellKey DynamicGridAoi::CellOf(const Position& p) const noexcept {
    const float cs = config_.cell_size > 0.0f ? config_.cell_size : 1.0f;
    return CellKey{static_cast<std::int32_t>(std::floor(p.x / cs)),
                   static_cast<std::int32_t>(std::floor(p.y / cs))};
}

bool DynamicGridAoi::IsValidCoord(const Position& p) const noexcept {
    if (!IsFinite(p.x) || !IsFinite(p.y) || !IsFinite(p.z)) return false;
    if (std::fabs(p.x) > kWorldHalf || std::fabs(p.y) > kWorldHalf ||
        std::fabs(p.z) > kWorldHalf)
        return false;
    return true;
}

float DynamicGridAoi::Dist3(const Position& a, const Position& b) noexcept {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void DynamicGridAoi::ComputeVisible(EntityId id, const Position& pos,
                                    std::vector<EntityId>& out) const {
    out.clear();
    const float r = config_.view_radius;
    if (r <= 0.0f) return;

    const float cs = config_.cell_size > 0.0f ? config_.cell_size : 1.0f;
    const int R = std::max(1, static_cast<int>(std::ceil(r / cs)));
    const CellKey c = CellOf(pos);

    // 邻域扫描：仅需 (2R+1)² 个格子，禁止全 Scene 遍历（§21）。
    for (int dy = -R; dy <= R; ++dy) {
        for (int dx = -R; dx <= R; ++dx) {
            const CellKey nk{c.x + static_cast<std::int32_t>(dx),
                             c.y + static_cast<std::int32_t>(dy)};
            auto it = cells_.find(nk);
            if (it == cells_.end()) continue;
            for (const EntityId eid : it->second) {
                if (eid == id) continue;  // 不含自身
                const auto er = entities_.find(eid);
                if (er == entities_.end()) continue;
                const Position ep{er->second.x, er->second.y, er->second.z, 0.0f};
                if (Dist3(pos, ep) <= r) out.push_back(eid);  // 每实体仅在一格，天然去重
            }
        }
    }
}

core::Result<void> DynamicGridAoi::Enter(EntityId id, const Position& pos) {
    if (!IsValidCoord(pos))
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                    "aoi enter: invalid coord (nan/oob)",
                                                    core::domain::kCore));
    if (entities_.find(id) != entities_.end())
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                    "aoi enter: duplicate id",
                                                    core::domain::kCore));
    if (entities_.size() >= config_.max_entities)
        return core::Result<void>::Fail(core::Error(core::ErrorCode::BUSY,
                                                    "aoi enter: capacity exceeded",
                                                    core::domain::kCore));

    const CellKey cell = CellOf(pos);
    EntityRecord rec{pos.x, pos.y, pos.z, cell};
    entities_.emplace(id, rec);
    cells_[cell].push_back(id);

    // 初始可见集（对称，无需缓存）：双向发出 entered 通知。
    std::vector<EntityId> vis;
    ComputeVisible(id, pos, vis);
    for (const EntityId v : vis) {
        if (notify_sink_) {
            notify_sink_(id, v, true);
            notify_sink_(v, id, true);
        }
    }
    return core::Result<void>::Ok();
}

core::Result<void> DynamicGridAoi::Leave(EntityId id) {
    const auto it = entities_.find(id);
    if (it == entities_.end())
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                    "aoi leave: id not found",
                                                    core::domain::kCore));
    const Position pos{it->second.x, it->second.y, it->second.z, 0.0f};
    const CellKey cell = it->second.cell;

    std::vector<EntityId> vis;
    ComputeVisible(id, pos, vis);
    for (const EntityId v : vis) {
        if (notify_sink_) {
            notify_sink_(id, v, false);
            notify_sink_(v, id, false);
        }
    }

    // 从格子移除（空格则删除以省内存）。
    auto cit = cells_.find(cell);
    if (cit != cells_.end()) {
        auto& vec = cit->second;
        vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
        if (vec.empty()) cells_.erase(cit);
    }
    entities_.erase(it);
    return core::Result<void>::Ok();
}

core::Result<MoveResult> DynamicGridAoi::Move(EntityId id, const Position& to) {
    auto it = entities_.find(id);
    if (it == entities_.end())
        return core::Result<MoveResult>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                          "aoi move: id not found",
                                                          core::domain::kCore));
    if (!IsValidCoord(to))
        return core::Result<MoveResult>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                          "aoi move: invalid coord (nan/oob)",
                                                          core::domain::kCore));

    const Position old_pos{it->second.x, it->second.y, it->second.z, 0.0f};
    const CellKey old_cell = it->second.cell;

    std::vector<EntityId> old_vis, new_vis;
    ComputeVisible(id, old_pos, old_vis);

    // 跨格更新（增量）：仅移除/插入涉及的两个格子。
    const CellKey new_cell = CellOf(to);
    if (new_cell != old_cell) {
        auto oit = cells_.find(old_cell);
        if (oit != cells_.end()) {
            auto& vec = oit->second;
            vec.erase(std::remove(vec.begin(), vec.end(), id), vec.end());
            if (vec.empty()) cells_.erase(oit);
        }
        cells_[new_cell].push_back(id);
        it->second.cell = new_cell;
    }
    it->second.x = to.x;
    it->second.y = to.y;
    it->second.z = to.z;

    ComputeVisible(id, to, new_vis);

    // 增量 diff（O(k)，排序后 set_difference，禁止全量重算，§15.6）。
    std::sort(old_vis.begin(), old_vis.end());
    std::sort(new_vis.begin(), new_vis.end());
    MoveResult res;
    std::set_difference(new_vis.begin(), new_vis.end(), old_vis.begin(), old_vis.end(),
                        std::back_inserter(res.entered));
    std::set_difference(old_vis.begin(), old_vis.end(), new_vis.begin(), new_vis.end(),
                        std::back_inserter(res.left));

    const float cs = config_.cell_size > 0.0f ? config_.cell_size : 1.0f;
    const int R = std::max(1, static_cast<int>(std::ceil(config_.view_radius / cs)));
    res.touched_cells = static_cast<std::uint32_t>((2 * R + 1) * (2 * R + 1));

    for (const EntityId e : res.entered) {
        if (notify_sink_) {
            notify_sink_(id, e, true);
            notify_sink_(e, id, true);
        }
    }
    for (const EntityId l : res.left) {
        if (notify_sink_) {
            notify_sink_(id, l, false);
            notify_sink_(l, id, false);
        }
    }
    return core::Result<MoveResult>::Ok(std::move(res));
}

core::Result<void> DynamicGridAoi::QueryVisible(EntityId id,
                                                std::vector<EntityId>& out) const {
    const auto it = entities_.find(id);
    if (it == entities_.end())
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                    "aoi query: id not found",
                                                    core::domain::kCore));
    const Position pos{it->second.x, it->second.y, it->second.z, 0.0f};
    const core::SteadyNs t0 = core::MonotonicClock::Now();
    ComputeVisible(id, pos, out);
    const core::SteadyNs t1 = core::MonotonicClock::Now();
    last_query_ns_ = static_cast<std::uint64_t>(t1 - t0);
    ++query_count_;
    return core::Result<void>::Ok();
}

core::Result<std::size_t> DynamicGridAoi::Broadcast(EntityId id,
                                                    std::span<const std::uint8_t> payload) {
    const auto it = entities_.find(id);
    if (it == entities_.end())
        return core::Result<std::size_t>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                           "aoi broadcast: id not found",
                                                           core::domain::kCore));
    const Position pos{it->second.x, it->second.y, it->second.z, 0.0f};
    const core::SteadyNs t0 = core::MonotonicClock::Now();

    std::vector<EntityId> targets;
    ComputeVisible(id, pos, targets);

    // 复用单一发送缓冲区（仅 resize 一次），禁止每目标单独分配/序列化（§15.5 / §21）。
    send_buf_.assign(payload.begin(), payload.end());

    std::size_t delivered = 0;
    if (broadcast_sink_) {
        for (const EntityId to : targets) {
            broadcast_sink_(id, to, std::span<const std::uint8_t>(send_buf_.data(),
                                                                  send_buf_.size()));
            ++delivered;
        }
    } else {
        delivered = targets.size();  // 无 sink：仅统计可达目标数
    }

    const core::SteadyNs t1 = core::MonotonicClock::Now();
    last_broadcast_ns_ = static_cast<std::uint64_t>(t1 - t0);
    return core::Result<std::size_t>::Ok(delivered);
}

AoiStats DynamicGridAoi::Stats() const noexcept {
    AoiStats s;
    s.entity_count = entities_.size();
    s.cell_count = cells_.size();
    s.last_query_ns = last_query_ns_;
    s.last_broadcast_ns = last_broadcast_ns_;
    s.query_count = query_count_;
    // avg_visible：对所有实体重算可见集求和（O(N·k)，仅 Stats() 调用，非热路径）。
    if (!entities_.empty()) {
        std::uint64_t sum = 0;
        for (const auto& kv : entities_) {
            const Position p{kv.second.x, kv.second.y, kv.second.z, 0.0f};
            std::vector<EntityId> tmp;
            const_cast<DynamicGridAoi*>(this)->ComputeVisible(kv.first, p, tmp);
            sum += tmp.size();
        }
        s.avg_visible = static_cast<std::size_t>(sum / entities_.size());
    }
    return s;
}

std::unique_ptr<IAoi> CreateDynamicGridAoi(AoiConfig config) {
    return std::make_unique<DynamicGridAoi>(std::move(config));
}

}  // namespace mmo::game::aoi
