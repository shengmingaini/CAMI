#include "mmo/core/error/error.h"

#include <cstring>

namespace mmo::core {

Error::Error(ErrorCode code, std::string_view message, std::string_view domain)
    : code_(code), inline_len_(0), domain_(domain) {
    inline_msg_[0] = '\0';
    if (message.size() <= kErrorMsgInline) {
        inline_len_ = static_cast<std::uint8_t>(message.size());
        if (inline_len_ > 0) {
            std::memcpy(inline_msg_, message.data(), inline_len_);
        }
        inline_msg_[inline_len_] = '\0';
    } else {
        // 超长 message 回退到堆（唯一允许的堆分配点，且非热路径短消息不触发）
        heap_msg_.assign(message);
    }
}

std::string_view Error::Message() const noexcept {
    if (heap_msg_.empty()) {
        return std::string_view(inline_msg_, inline_len_);
    }
    return heap_msg_;
}

std::string Error::ToString() const {
    std::string out;
    const std::string_view msg = Message();
    out.reserve(domain_.size() + 16 + msg.size());
    out += domain_;
    out += '/';
    out += ::mmo::core::ToString(code_);
    out += ": ";
    out += msg;
    return out;
}

}  // namespace mmo::core
