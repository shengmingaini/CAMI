#pragma once

namespace cami::gateway::router {

// 无 GTest、无真实 socket；被 tests/unit/skeleton_layer_check.cpp 调用，
// 作为 CI 始终编译 + 功能验证的路由模块证据。
// 返回 true 表示通过。验证：路由命中/确定性、节点增减迁移 <10%、
// 虚拟节点均衡、热更新（reload_backends 替换环）。
// 返回约定与兄弟模块一致：true = 成功。
bool router_selfcheck();

} // namespace cami::gateway::router
