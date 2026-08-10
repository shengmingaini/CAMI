#include "gateway/codec/frame_decoder.h"

namespace cami {
namespace gateway {
namespace codec {

namespace {
inline std::uint32_t read_u32_le(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0])
         | (static_cast<std::uint32_t>(p[1]) << 8)
         | (static_cast<std::uint32_t>(p[2]) << 16)
         | (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

FrameDecoder::FrameDecoder(std::size_t max_frame_size)
    : max_frame_size_(max_frame_size) {}

void FrameDecoder::reset() { buf_.clear(); }

DecodeResult FrameDecoder::consume(
    const std::uint8_t* data, std::size_t len,
    const std::function<void(std::vector<std::uint8_t>&&)>& on_frame) {
    if (len > 0 && data != nullptr) {
        buf_.insert(buf_.end(), data, data + len);
    }

    std::size_t i = 0;
    bool oversized = false;
    while (buf_.size() - i >= kFrameHeaderSize) {
        const std::uint32_t declared = read_u32_le(&buf_[i]);
        if (declared > max_frame_size_) {
            oversized = true;  // 流失同步：长度超出上限，拒绝并清空。
            break;
        }
        const std::size_t needed = kFrameHeaderSize + static_cast<std::size_t>(declared);
        if (buf_.size() - i < needed) {
            break;  // 半包：数据不足，等待后续字节。
        }
        std::vector<std::uint8_t> payload(
            buf_.data() + i + kFrameHeaderSize,
            buf_.data() + i + needed);
        on_frame(std::move(payload));  // 粘包：循环继续切下一帧。
        i += needed;
    }

    if (oversized) {
        buf_.clear();  // 失同步后无法信任残流，清空交由调用方关闭连接。
        return DecodeResult::kOversized;
    }
    if (i > 0) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(i));  // 丢弃已消费前缀。
    }
    return DecodeResult::kOk;
}

bool looks_like_flatbuffer(const std::uint8_t* data, std::size_t len) {
    if (len < 4 || data == nullptr) return false;
    const std::uint32_t off = read_u32_le(data);
    // 根偏移 >= 4 且指向的根表（含自身 4 字节 uoffset 字段）落在缓冲内。
    return off >= 4 && (static_cast<std::uint64_t>(off) + 4) <= len;
}

}  // namespace codec
}  // namespace gateway
}  // namespace cami
