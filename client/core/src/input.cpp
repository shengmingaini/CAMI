/// TASK-034 · Input 实现。

#include "mmo/client/input.h"

namespace mmo { namespace client {

void Input::SetKey(std::uint32_t code, bool down) {
    if (down) {
        if (held_.find(code) == held_.end()) just_pressed_.insert(code);
        held_.insert(code);
    } else {
        held_.erase(code);
    }
}

InputSnapshot Input::Sample() {
    InputSnapshot s;
    s.held = held_;
    s.just_pressed = just_pressed_;
    just_pressed_.clear();
    return s;
}

bool Input::IsHeld(std::uint32_t code) const {
    return held_.find(code) != held_.end();
}

}}  // namespace mmo::client
