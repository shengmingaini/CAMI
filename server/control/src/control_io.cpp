// server/control/src/control_io.cpp — 内部序列化实现（详见 control_io.h）

#include "control_io.h"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mmo::control::detail {

namespace {

constexpr std::string_view kDomain = "control";
constexpr char kFS = '\x1f';  // unit separator

inline core::Error IoErr(std::string_view msg) {
    return core::Error(core::ErrorCode::INTERNAL_ERROR, msg, kDomain);
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t pos = 0;
    while (pos <= s.size()) {
        const std::size_t found = s.find(sep, pos);
        if (found == std::string_view::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, found - pos));
        pos = found + 1;
    }
    return out;
}

template <typename T>
core::Result<T> ParseInt(std::string_view s) {
    T v{};
    const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || p != s.data() + s.size()) {
        return core::Result<T>::Fail(IoErr("bad integer field"));
    }
    return core::Result<T>::Ok(v);
}

core::Result<std::uint8_t> ParseUint8(std::string_view s) {
    auto r = ParseInt<std::uint32_t>(s);
    if (!r.HasValue() || r.Value() > 255) {
        return core::Result<std::uint8_t>::Fail(IoErr("bad uint8 field"));
    }
    return core::Result<std::uint8_t>::Ok(static_cast<std::uint8_t>(r.Value()));
}

}  // namespace

std::string SerializeNodes(const std::vector<ControlNodeInfo>& nodes) {
    std::string out;
    for (const auto& n : nodes) {
        if (!out.empty()) out.push_back('\n');
        out += std::to_string(n.node_id);
        out.push_back(kFS);
        out += std::to_string(static_cast<int>(n.role));
        out.push_back(kFS);
        out += n.addr;
        out.push_back(kFS);
        out += std::to_string(n.capacity);
        out.push_back(kFS);
        out += std::to_string(n.load);
        out.push_back(kFS);
        out += std::to_string(n.player_count);
        out.push_back(kFS);
        out += std::to_string(n.tick_p99_ms);
        out.push_back(kFS);
        out += std::to_string(static_cast<int>(n.status));
        out.push_back(kFS);
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                n.last_heartbeat.time_since_epoch())
                .count();
        out += std::to_string(ns);
    }
    return out;
}

core::Result<std::vector<ControlNodeInfo>> DeserializeNodes(std::string_view text) {
    std::vector<ControlNodeInfo> out;
    std::size_t pos = 0;
    const std::size_t total = text.size();
    while (pos < total) {
        const std::size_t eol = text.find('\n', pos);
        const std::string_view rec =
            (eol == std::string_view::npos) ? text.substr(pos)
                                            : text.substr(pos, eol - pos);
        pos = (eol == std::string_view::npos) ? total : eol + 1;
        if (rec.empty()) continue;

        const std::vector<std::string_view> f = Split(rec, kFS);
        if (f.size() != 9) return core::Result<std::vector<ControlNodeInfo>>::Fail(IoErr("node field count"));

        ControlNodeInfo info{};
        auto id = ParseInt<std::uint32_t>(f[0]);  if (!id)  return core::Result<std::vector<ControlNodeInfo>>::Fail(id.Err());
        auto role = ParseUint8(f[1]);              if (!role) return core::Result<std::vector<ControlNodeInfo>>::Fail(role.Err());
        auto cap = ParseInt<std::uint32_t>(f[3]); if (!cap)  return core::Result<std::vector<ControlNodeInfo>>::Fail(cap.Err());
        auto load = ParseInt<std::uint32_t>(f[4]);if (!load) return core::Result<std::vector<ControlNodeInfo>>::Fail(load.Err());
        auto pc = ParseInt<std::uint32_t>(f[5]);  if (!pc)   return core::Result<std::vector<ControlNodeInfo>>::Fail(pc.Err());
        auto t9 = ParseInt<std::uint32_t>(f[6]);  if (!t9)   return core::Result<std::vector<ControlNodeInfo>>::Fail(t9.Err());
        auto st = ParseUint8(f[7]);                if (!st)   return core::Result<std::vector<ControlNodeInfo>>::Fail(st.Err());
        auto ns = ParseInt<std::int64_t>(f[8]);    if (!ns)   return core::Result<std::vector<ControlNodeInfo>>::Fail(ns.Err());

        info.node_id = id.Value();
        info.role = static_cast<ControlRole>(role.Value());
        info.addr = std::string(f[2]);
        info.capacity = cap.Value();
        info.load = load.Value();
        info.player_count = pc.Value();
        info.tick_p99_ms = t9.Value();
        info.status = static_cast<NodeStatus>(st.Value());
        info.last_heartbeat = core::SteadyTime{std::chrono::nanoseconds{ns.Value()}};
        out.push_back(std::move(info));
    }
    return core::Result<std::vector<ControlNodeInfo>>::Ok(std::move(out));
}

std::string SerializeConfig(std::uint64_t version, std::string_view snapshot) {
    std::string out = std::to_string(version);
    out.push_back(kFS);
    out += std::to_string(snapshot.size());
    out.push_back(kFS);
    out.append(snapshot.data(), snapshot.size());
    return out;
}

core::Result<std::pair<std::uint64_t, std::string>> DeserializeConfig(std::string_view text) {
    const std::size_t p1 = text.find(kFS);
    if (p1 == std::string_view::npos) return core::Result<std::pair<std::uint64_t, std::string>>::Fail(IoErr("config missing sep"));
    const std::size_t p2 = text.find(kFS, p1 + 1);
    if (p2 == std::string_view::npos) return core::Result<std::pair<std::uint64_t, std::string>>::Fail(IoErr("config missing len"));

    auto ver = ParseInt<std::uint64_t>(text.substr(0, p1));
    if (!ver) return core::Result<std::pair<std::uint64_t, std::string>>::Fail(ver.Err());
    auto len = ParseInt<std::uint64_t>(text.substr(p1 + 1, p2 - p1 - 1));
    if (!len) return core::Result<std::pair<std::uint64_t, std::string>>::Fail(len.Err());

    const std::size_t body = p2 + 1;
    if (text.size() != body + len.Value()) {
        return core::Result<std::pair<std::uint64_t, std::string>>::Fail(IoErr("config length mismatch"));
    }
    std::string snap(text.data() + body, len.Value());
    return core::Result<std::pair<std::uint64_t, std::string>>::Ok(std::make_pair(ver.Value(), std::move(snap)));
}

}  // namespace mmo::control::detail
