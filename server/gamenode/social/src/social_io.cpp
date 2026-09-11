// server/gamenode/social/src/social_io.cpp — TASK-039 社交对象序列化（内部）
//
// 持久化格式为模块内部约定，经 IDataStore::payload 存储。无第三方 JSON 依赖。

#include "social_io.h"

#include <string>
#include <vector>

#include "mmo/core/error/error.h"

namespace mmo::game::social {

namespace {
constexpr char kUnitSep = '\x1f';  // 字段分隔符（正文不出现）

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true) {
        const auto pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}
}  // namespace

// ---- Guild ----

std::string SerializeGuild(const Guild& g) {
    std::string s = g.name;
    s += '|';
    s += std::to_string(g.leader);
    s += '|';
    s += std::to_string(static_cast<unsigned long long>(g.member_count));
    s += '|';
    for (std::size_t i = 0; i < g.members.size(); ++i) {
        if (i != 0) s += ',';
        s += std::to_string(g.members[i]);
    }
    return s;
}

mmo::core::Result<Guild> DeserializeGuild(std::string_view s) {
    auto parts = Split(s, '|');
    if (parts.size() != 4) {
        return mmo::core::Result<Guild>::Fail(
            mmo::core::Error(mmo::core::ErrorCode::INTERNAL_ERROR, "bad guild payload",
                             mmo::core::domain::kData));
    }
    Guild g;
    try {
        g.name = std::string(parts[0]);
        g.leader = static_cast<player_id>(std::stoull(std::string(parts[1])));
        g.member_count = static_cast<std::uint32_t>(std::stoul(std::string(parts[2])));
        auto members = Split(parts[3], ',');
        if (!(members.size() == 1 && members[0].empty())) {
            for (auto m : members) {
                if (m.empty()) continue;
                g.members.push_back(static_cast<player_id>(std::stoull(std::string(m))));
            }
        }
    } catch (...) {
        return mmo::core::Result<Guild>::Fail(
            mmo::core::Error(mmo::core::ErrorCode::INTERNAL_ERROR, "bad guild payload",
                             mmo::core::domain::kData));
    }
    return mmo::core::Result<Guild>::Ok(g);
}

// ---- Mail ----

std::string SerializeMail(const Mail& m) {
    std::string s;
    s += std::to_string(m.id);
    s += kUnitSep;
    s += std::to_string(m.from);
    s += kUnitSep;
    s += std::to_string(m.to);
    s += kUnitSep;
    s += m.subject;
    s += kUnitSep;
    s += m.body;
    s += kUnitSep;
    s += (m.has_attachment ? '1' : '0');
    s += kUnitSep;
    s += std::to_string(m.attachment_amount);
    s += kUnitSep;
    s += (m.claimed ? '1' : '0');
    s += kUnitSep;
    s += std::to_string(m.created_at);
    s += kUnitSep;
    s += std::to_string(m.expires_at);
    return s;
}

mmo::core::Result<Mail> DeserializeMail(std::string_view s) {
    auto parts = Split(s, kUnitSep);
    if (parts.size() != 10) {
        return mmo::core::Result<Mail>::Fail(
            mmo::core::Error(mmo::core::ErrorCode::INTERNAL_ERROR, "bad mail payload",
                             mmo::core::domain::kData));
    }
    Mail m;
    try {
        m.id = static_cast<mail_id>(std::stoull(std::string(parts[0])));
        m.from = static_cast<player_id>(std::stoull(std::string(parts[1])));
        m.to = static_cast<player_id>(std::stoull(std::string(parts[2])));
        m.subject = std::string(parts[3]);
        m.body = std::string(parts[4]);
        m.has_attachment = (parts[5] == "1");
        m.attachment_amount = std::stoull(std::string(parts[6]));
        m.claimed = (parts[7] == "1");
        m.created_at = std::stoull(std::string(parts[8]));
        m.expires_at = std::stoull(std::string(parts[9]));
    } catch (...) {
        return mmo::core::Result<Mail>::Fail(
            mmo::core::Error(mmo::core::ErrorCode::INTERNAL_ERROR, "bad mail payload",
                             mmo::core::domain::kData));
    }
    return mmo::core::Result<Mail>::Ok(m);
}

// ---- Friends ----

std::string SerializeFriends(const std::set<player_id>& s) {
    std::string out;
    bool first = true;
    for (auto p : s) {
        if (!first) out += ',';
        first = false;
        out += std::to_string(p);
    }
    return out;
}

mmo::core::Result<std::set<player_id>> DeserializeFriends(std::string_view s) {
    std::set<player_id> out;
    auto parts = Split(s, ',');
    if (!(parts.size() == 1 && parts[0].empty())) {
        for (auto p : parts) {
            if (p.empty()) continue;
            try {
                out.insert(static_cast<player_id>(std::stoull(std::string(p))));
            } catch (...) {
                return mmo::core::Result<std::set<player_id>>::Fail(
                    mmo::core::Error(mmo::core::ErrorCode::INTERNAL_ERROR, "bad friends payload",
                                     mmo::core::domain::kData));
            }
        }
    }
    return mmo::core::Result<std::set<player_id>>::Ok(out);
}

}  // namespace mmo::game::social
