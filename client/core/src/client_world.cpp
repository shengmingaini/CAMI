/// TASK-034 · ClientWorld 实现（快照编解码 + 插值）。

#include "mmo/client/client_world.h"

#include <cstring>

namespace mmo { namespace client {

void ClientWorld::ApplySnapshot(const WorldSnapshot& snap) {
    for (const auto& pose : snap.entities) {
        auto& st = entities_[pose.id];
        if (st.has_curr) {
            st.prev = st.curr;
            st.has_prev = true;
        }
        st.curr = pose;
        st.has_curr = true;
        st.last_ts = pose.ts_ms;
    }
}

std::vector<std::uint8_t> ClientWorld::EncodeSnapshot(const WorldSnapshot& snap) {
    std::vector<std::uint8_t> out;
    out.resize(sizeof(std::int64_t) + sizeof(std::uint32_t) +
               snap.entities.size() * (sizeof(EntityId) + 7 * sizeof(float) + sizeof(std::int64_t)));
    std::size_t off = 0;
    auto put_i64 = [&](std::int64_t v) {
        std::memcpy(out.data() + off, &v, sizeof(v)); off += sizeof(v);
    };
    auto put_u64 = [&](std::uint64_t v) {
        std::memcpy(out.data() + off, &v, sizeof(v)); off += sizeof(v);
    };
    auto put_f32 = [&](float v) {
        std::memcpy(out.data() + off, &v, sizeof(v)); off += sizeof(v);
    };
    put_i64(snap.server_time_ms);
    const std::uint32_t n = static_cast<std::uint32_t>(snap.entities.size());
    std::memcpy(out.data() + off, &n, sizeof(n)); off += sizeof(n);
    for (const auto& e : snap.entities) {
        put_u64(static_cast<std::uint64_t>(e.id));
        put_f32(e.pos.x); put_f32(e.pos.y); put_f32(e.pos.z);
        put_f32(e.vel.x); put_f32(e.vel.y); put_f32(e.vel.z);
        put_f32(e.heading);
        put_i64(e.ts_ms);
    }
    return out;
}

WorldSnapshot ClientWorld::DecodeSnapshot(std::string_view bytes) {
    WorldSnapshot snap;
    if (bytes.size() < sizeof(std::int64_t) + sizeof(std::uint32_t)) return snap;
    std::size_t off = 0;
    auto get_i64 = [&]() -> std::int64_t {
        std::int64_t v; std::memcpy(&v, bytes.data() + off, sizeof(v)); off += sizeof(v); return v;
    };
    auto get_u64 = [&]() -> std::uint64_t {
        std::uint64_t v; std::memcpy(&v, bytes.data() + off, sizeof(v)); off += sizeof(v); return v;
    };
    auto get_f32 = [&]() -> float {
        float v; std::memcpy(&v, bytes.data() + off, sizeof(v)); off += sizeof(v); return v;
    };
    snap.server_time_ms = get_i64();
    std::uint32_t n; std::memcpy(&n, bytes.data() + off, sizeof(n)); off += sizeof(n);
    snap.entities.reserve(n);
    for (std::uint32_t i = 0; i < n && off + sizeof(EntityId) + 7 * sizeof(float) + sizeof(std::int64_t) <= bytes.size(); ++i) {
        EntityPose e;
        e.id = static_cast<EntityId>(get_u64());
        e.pos.x = get_f32(); e.pos.y = get_f32(); e.pos.z = get_f32();
        e.vel.x = get_f32(); e.vel.y = get_f32(); e.vel.z = get_f32();
        e.heading = get_f32();
        e.ts_ms = get_i64();
        snap.entities.push_back(e);
    }
    return snap;
}

std::vector<EntityRender> ClientWorld::Interpolate(std::int64_t render_time_ms,
                                                   std::int64_t extrapolation_limit_ms) {
    std::vector<EntityRender> out;
    out.reserve(entities_.size());
    for (auto& kv : entities_) {
        EntityRender r;
        r.id = kv.first;
        const EntityState& st = kv.second;
        if (!st.has_curr) continue;
        if (st.has_prev && st.curr.ts_ms > st.prev.ts_ms) {
            const double span = static_cast<double>(st.curr.ts_ms - st.prev.ts_ms);
            double alpha = static_cast<double>(render_time_ms - st.prev.ts_ms) / span;
            if (alpha < 0.0) alpha = 0.0;
            if (alpha > 1.0) alpha = 1.0;
            r.pos.x = st.prev.pos.x + (st.curr.pos.x - st.prev.pos.x) * static_cast<float>(alpha);
            r.pos.y = st.prev.pos.y + (st.curr.pos.y - st.prev.pos.y) * static_cast<float>(alpha);
            r.pos.z = st.prev.pos.z + (st.curr.pos.z - st.prev.pos.z) * static_cast<float>(alpha);
            r.heading = st.prev.heading + (st.curr.heading - st.prev.heading) * static_cast<float>(alpha);
        } else {
            r.pos = st.curr.pos;
            r.heading = st.curr.heading;
        }
        // 外推上限：最新快照比 render_time 旧超过 limit -> 冻结
        if (st.curr.ts_ms + extrapolation_limit_ms < render_time_ms) {
            r.frozen = true;
            ++frozen_count_;
        }
        out.push_back(r);
    }
    return out;
}

}}  // namespace mmo::client
