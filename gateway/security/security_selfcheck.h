#pragma once

namespace cami::gateway::security {

// 无 GTest、无真实 socket；被 tests/unit/skeleton_layer_check.cpp 调用，
// 作为 CI 始终编译 + 功能验证的安全模块证据。
// 返回 true 表示通过。CAMI_BUILD_MODULES=OFF 下加密路径标记 [DISABLED] 但仍通过
// （token 路径不依赖 OpenSSL，始终可测）。返回约定与兄弟模块一致：true=成功。
bool security_selfcheck();

} // namespace cami::gateway::security
