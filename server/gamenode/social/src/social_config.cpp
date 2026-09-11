// server/gamenode/social/src/social_config.cpp — TASK-039 配置解析（极简字段扫描）

#include "mmo/game/social/social_config.h"

#include <cctype>
#include <string_view>

namespace mmo::game::social {

namespace {
bool ExtractUint(std::string_view text, std::string_view key, std::uint64_t& out) {
    const auto pos = text.find(key);
    if (pos == std::string_view::npos) return false;
    const auto colon = text.find(':', pos + key.size());
    if (colon == std::string_view::npos) return false;
    std::size_t i = colon + 1;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    std::uint64_t v = 0;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        v = v * 10ULL + static_cast<std::uint64_t>(text[i] - '0');
        ++i;
    }
    out = v;
    return true;
}
}  // namespace

SocialConfig ParseSocialConfig(std::string_view text) {
    SocialConfig cfg;
    std::uint64_t v = 0;
    if (ExtractUint(text, "party_size_max", v)) {
        cfg.party_size_max = static_cast<std::uint32_t>(v);
    }
    if (ExtractUint(text, "guild_member_max", v)) {
        cfg.guild_member_max = static_cast<std::uint32_t>(v);
    }
    if (ExtractUint(text, "mail_ttl_seconds", v)) {
        cfg.mail_ttl_seconds = v;
    }

    const auto wb = text.find("world_chat_enabled");
    if (wb != std::string_view::npos) {
        const auto colon = text.find(':', wb);
        if (colon != std::string_view::npos) {
            const auto t = text.find("true", colon);
            const auto f = text.find("false", colon);
            if (t != std::string_view::npos && (f == std::string_view::npos || t < f)) {
                cfg.world_chat_enabled = true;
            } else if (f != std::string_view::npos) {
                cfg.world_chat_enabled = false;
            }
        }
    }
    return cfg;
}

}  // namespace mmo::game::social
