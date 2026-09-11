#pragma once

/// TASK-039 · Social System 公开类型（§7 / §8 / §15）。
///
/// 复用上游类型，禁止重新定义：
///   · player_id / scene_id / node_id 来自 mmo::game（scene_id.h，TASK-012）
///   · 其余社交域 ID 为本模块自有 uint64 别名（与上游同宽，便于持久化键拼接）

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "mmo/game/scene/scene_id.h"  // PlayerId / SceneId / NodeId

namespace mmo::game::social {

using player_id = mmo::game::PlayerId;    // std::uint64_t
using scene_id  = mmo::game::SceneId;     // std::uint64_t
using node_id   = mmo::game::NodeId;      // std::uint32_t

using party_id = std::uint64_t;
using guild_id = std::uint64_t;
using mail_id  = std::uint64_t;

/// 社交操作枚举（§8 / §21）。新增社交关系类型必须加枚举值 + 注册表 handler，
/// 禁止在 handler 里 switch 硬编码穷举。PartyCreate 为 §15.2 组队创建所需，
/// 与规格 12 项同为注册表扩展点。
enum class SocialOp : std::uint8_t {
    Invite = 0,
    Accept,
    Decline,
    Leave,
    Kick,
    Promote,
    PartyCreate,    // §15.2 组队创建
    AddFriend,
    RemoveFriend,
    SendMail,
    ClaimMail,
    GuildCreate,
    GuildJoin,
};

enum class ChatChannel : std::uint8_t {
    Private = 0,
    Party,
    Guild,
    World,
};

struct Party {
    party_id id{};
    std::vector<player_id> members;
    player_id leader{};
    scene_id home{};
};

struct Guild {
    guild_id id{};
    std::string name;
    player_id leader{};
    std::uint32_t member_count{};
    std::vector<player_id> members;
};

struct Mail {
    mail_id id{};
    player_id from{};
    player_id to{};
    std::string subject;
    std::string body;
    bool has_attachment{};
    std::uint64_t attachment_amount{};
    bool claimed{};
    std::uint64_t created_at{};
    std::uint64_t expires_at{};
};

/// 统一命令（经注册表分发，禁 switch 硬编码）。公开方法负责构造本结构并 Dispatch。
struct SocialCommand {
    SocialOp op{};
    player_id actor{};
    player_id target{};
    party_id pid{};
    guild_id gid{};
    mail_id mid{};
    std::string payload;    // 复用字段：公会名 / 邮件正文
    std::string subject;    // 邮件主题
    std::uint32_t max_members{};
    std::uint64_t amount{};
    bool has_attachment{};
};

/// 注册表处理结果（variant 式，避免 std::variant 头依赖）。
struct SocialOutcome {
    party_id as_party{};
    guild_id as_guild{};
    mail_id as_mail{};
    bool has_party{};
    bool has_guild{};
    bool has_mail{};
};

}  // namespace mmo::game::social
