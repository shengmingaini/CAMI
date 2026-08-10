#pragma once

namespace cami {
namespace gateway {
namespace heartbeat {

// CI 机器证据：不依赖 GTest、不绑定任何端口。
// 覆盖：注册/活动保活/超时踢线/显式注销无泄漏/空闲回收二次阈值/踢线回调计数。
bool heartbeat_selfcheck();

}  // namespace heartbeat
}  // namespace gateway
}  // namespace cami
