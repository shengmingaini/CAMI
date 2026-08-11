#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cami::gateway::router {

// 路由结果：kRouted=命中后端；kNoBackend=环为空无可用节点。
enum class RouteResult : int {
    kRouted = 0,
    kNoBackend = 1,
};

struct RouterConfig {
    int virtual_nodes_per_node = 100;  // 每物理节点虚拟节点数：越多分布越均衡、迁移越平滑
};

} // namespace cami::gateway::router
