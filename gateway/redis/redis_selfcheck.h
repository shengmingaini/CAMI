#pragma once

namespace cami::gateway::redis {

// 无 GTest、无真实 socket / Redis；被 tests/unit/skeleton_layer_check.cpp 调用，
// 作为 CI 始终编译 + 功能验证的 Redis 模块证据（OFF 下验证内存后端 + 分片路由）。
// 返回 true 表示通过。返回约定与兄弟模块一致：true = 成功。
bool redis_selfcheck();

} // namespace cami::gateway::redis
