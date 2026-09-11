#pragma once

/// TASK-039 · 社交事件（§7）。四类事件均为小值类型，经 EventBus 发布（TASK-007），
/// 通过 typeid 自动注册，无需宏。社交写操作统一发出对应事件，禁止直改 Role/Scene。

#include <cstdint>
#include <string>

#include "mmo/game/social/social_types.h"

namespace mmo::game::social {

/// 组队事件：创建 / 邀请 / 接受 / 拒绝 / 离开 / 踢人 / 队长转移。
struct PartyEvent {
    SocialOp op{};
    party_id pid{};
    player_id actor{};
    player_id target{};
    scene_id home{};
};

/// 公会事件：创建 / 加入 / 离开（op 复用 SocialOp）。
struct GuildEvent {
    SocialOp op{};
    guild_id gid{};
    player_id actor{};
    player_id target{};
    std::string name;
};

/// 聊天事件：四频道统一走本事件。世界频道只发一条（由 Gateway 订阅表路由，
/// 绝不在 SocialSystem 内做全服 O(N) 玩家遍历广播，见 §21）。
struct ChatEvent {
    ChatChannel channel{};
    player_id from{};
    player_id to{};     // 私聊目标；其余频道为 0
    guild_id gid{};     // 公会频道目标
    party_id pid{};     // 队伍频道目标
    std::string message;
};

/// 邮件事件：发送 / 领取。
struct MailEvent {
    SocialOp op{};
    mail_id mid{};
    player_id from{};
    player_id to{};
};

}  // namespace mmo::game::social
