// server/dataservice/include/mmo/data/redis/keys.h
//
// TASK-027 · 键空间规范（§8）。统一前缀，禁止各模块自造键名。
//   Session   sess:{session_id}
//   Player路由 route:player:{player_id}
//   Scene路由 route:scene:{scene_id}
//   角色缓存  cache:char:{char_id}
//   背包缓存  cache:inv:{char_id}

#pragma once

#include <string>

#include "mmo/data/record.h"

namespace mmo::data::redis {

inline DataKey SessionKey(std::string_view session_id) {
    return std::string("sess:") + std::string(session_id);
}

inline DataKey PlayerRouteKey(std::string_view player_id) {
    return std::string("route:player:") + std::string(player_id);
}

inline DataKey SceneRouteKey(std::string_view scene_id) {
    return std::string("route:scene:") + std::string(scene_id);
}

inline DataKey CharCacheKey(std::string_view char_id) {
    return std::string("cache:char:") + std::string(char_id);
}

inline DataKey InvCacheKey(std::string_view char_id) {
    return std::string("cache:inv:") + std::string(char_id);
}

}  // namespace mmo::data::redis
