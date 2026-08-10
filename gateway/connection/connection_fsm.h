#pragma once

#include <cstdint>

namespace cami {
namespace gateway {
namespace connection {

// 连接生命周期状态机 [PROTOTYPE]
// 状态边界：Idle(构造) → Connecting(建连中) → Handshaking(握手/加密协商, 留 hook 给 codec/security)
//         → Established(可收发) → Closing(关闭中) → Closed(可析构)；
//         任意非终态遇异常 → Error → Closed。
enum class ConnectionState : uint8_t {
    kIdle = 0,
    kConnecting,
    kHandshaking,
    kEstablished,
    kClosing,
    kClosed,
    kError,
};

// 编译期非法迁移直接返回 false，连接类据此拒绝并转 kError，禁止静默状态错乱。
bool can_transition(ConnectionState from, ConnectionState to) noexcept;

// 终态判定：Closed 不再迁移；Error 仅允许 → Closed。
inline bool is_terminal(ConnectionState s) noexcept { return s == ConnectionState::kClosed; }

const char* to_string(ConnectionState s) noexcept;

}  // namespace connection
}  // namespace gateway
}  // namespace cami
