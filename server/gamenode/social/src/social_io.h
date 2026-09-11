#pragma once

/// TASK-039 · 社交对象序列化（内部，供 src/ 使用，不进公开接口）。
/// 持久化格式为模块内部约定，经 IDataStore 的 payload 字段存储。

#include <set>
#include <string>
#include <string_view>

#include "mmo/core/error/result.h"
#include "mmo/game/social/social_types.h"

namespace mmo::game::social {

std::string SerializeGuild(const Guild& g);
mmo::core::Result<Guild> DeserializeGuild(std::string_view s);

std::string SerializeMail(const Mail& m);
mmo::core::Result<Mail> DeserializeMail(std::string_view s);

std::string SerializeFriends(const std::set<player_id>& s);
mmo::core::Result<std::set<player_id>> DeserializeFriends(std::string_view s);

}  // namespace mmo::game::social
