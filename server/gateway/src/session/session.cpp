// server/gateway/src/session/session.cpp — TASK-009 §15.1

#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

const char* ToString(SessionState state) noexcept {
    switch (state) {
        case SessionState::Connecting:     return "Connecting";
        case SessionState::Authenticating: return "Authenticating";
        case SessionState::Active:         return "Active";
        case SessionState::Suspended:      return "Suspended";
        case SessionState::Closing:        return "Closing";
        case SessionState::Closed:         return "Closed";
    }
    return "UNKNOWN";  // 未知值禁止崩溃（§21）
}

}  // namespace mmo::gateway
