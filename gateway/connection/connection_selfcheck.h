#pragma once

namespace cami {
namespace gateway {
namespace connection {

// 机器可验证证据：连接模块 FSM 迁移合法性 + IoContextPool 启停 + 轮询分配。
// 不绑定真实端口，CI 安全。被 tests/unit/skeleton_layer_check.cpp 调用。
bool connection_selfcheck();

}  // namespace connection
}  // namespace gateway
}  // namespace cami
