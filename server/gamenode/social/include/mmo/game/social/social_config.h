#pragma once

/// TASK-039 · SocialSystem 运行配置（§14 / config/gameplay/social.json）。

#include <cstdint>
#include <string_view>

namespace mmo::game::social {

struct SocialConfig {
    std::uint32_t party_size_max = 5;          // 队伍成员上限（默认 5）
    std::uint32_t guild_member_max = 100;      // 公会成员上限
    std::uint64_t mail_ttl_seconds = 604800ULL; // 邮件有效期（默认 7 天）
    bool world_chat_enabled = true;            // 世界频道开关
};

/// 从 social.json 文本解析（极简字段扫描，无第三方 JSON 依赖）。
/// 缺失 / 解析失败的字段回退默认值；不抛异常。
SocialConfig ParseSocialConfig(std::string_view text);

}  // namespace mmo::game::social
