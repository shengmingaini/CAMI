#include "gateway/connection/connection_fsm.h"

namespace cami {
namespace gateway {
namespace connection {

bool can_transition(ConnectionState from, ConnectionState to) noexcept {
    if (from == to) return false;  // 不允许自环
    switch (from) {
        case ConnectionState::kIdle:
            return to == ConnectionState::kConnecting;
        case ConnectionState::kConnecting:
            return to == ConnectionState::kHandshaking
                || to == ConnectionState::kError
                || to == ConnectionState::kClosed;
        case ConnectionState::kHandshaking:
            return to == ConnectionState::kEstablished
                || to == ConnectionState::kError
                || to == ConnectionState::kClosed;
        case ConnectionState::kEstablished:
            return to == ConnectionState::kClosing
                || to == ConnectionState::kError
                || to == ConnectionState::kClosed;
        case ConnectionState::kClosing:
            return to == ConnectionState::kClosed
                || to == ConnectionState::kError;
        case ConnectionState::kError:
            return to == ConnectionState::kClosed;
        case ConnectionState::kClosed:
            return false;  // 终态
    }
    return false;
}

const char* to_string(ConnectionState s) noexcept {
    switch (s) {
        case ConnectionState::kIdle: return "Idle";
        case ConnectionState::kConnecting: return "Connecting";
        case ConnectionState::kHandshaking: return "Handshaking";
        case ConnectionState::kEstablished: return "Established";
        case ConnectionState::kClosing: return "Closing";
        case ConnectionState::kClosed: return "Closed";
        case ConnectionState::kError: return "Error";
    }
    return "Unknown";
}

}  // namespace connection
}  // namespace gateway
}  // namespace cami
