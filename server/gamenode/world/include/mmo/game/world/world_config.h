#pragma once

/// TASK-020 · World / Instance 配置加载器（§15.1 / §16 / §21 Forbidden）。
///
/// 从 config/gameplay/world/*.json 读取实例定义与 OpenWorld 定义，全部字段来自配置，
/// 失败返回错误，禁止用默认值静默生成（§19）。自带最小递归下降 JSON 解析器
/// （顶层为含 "instances" / "worlds" 键的对象），不依赖第三方库。
///
/// 内存归属：InstanceDef 的 name / scene_asset 为 string_view，指向 WorldConfigBundle::pool
/// （std::deque，元素地址稳定），bundle 被持有方（InstanceManager / WorldManager）拥有，
/// 保证 string_view 在实例生命周期内有效（§15.1 配置化 + 零悬垂）。

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include "mmo/core/error/result.h"
#include "mmo/game/world/instance_def.h"

namespace mmo::game::world {

/// OpenWorld（常驻主世界）静态定义。
struct WorldDef {
    std::uint32_t id{0};
    std::string   name;
    std::string   scene_asset;
};

/// 配置包：拥有字符串池 + 实例/世界定义表。由加载器产出，被管理器持有。
struct WorldConfigBundle {
    std::deque<std::string> pool;                          // string_view 生命期宿主
    std::unordered_map<std::uint32_t, InstanceDef> instances;
    std::unordered_map<std::uint32_t, WorldDef>    worlds;
};

/// 加载目录下所有 *.json（按文件名排序），合并 instances / worlds。
/// 校验：def_id / world id 唯一；type 合法；max_players>=1；min_players<=max_players。
/// 任一不合法返回错误（§16「缺字段报错、引用不存在的 Scene 报错」）。
core::Result<WorldConfigBundle> LoadWorldConfig(std::string_view dir);

}  // namespace mmo::game::world
