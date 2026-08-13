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

// 惰性 compact 阈值：已消费游标累积超过 4KB，或已消费部分占缓冲过半时，
// 一次性 erase 前缀（memmove 一次），避免每帧 O(n) 前缀删除。
constexpr std::size_t kCompactThreshold = 4096;

}  // namespace

FrameDecoder::FrameDecoder(std::size_t max_frame_size)
    : max_frame_size_(max_frame_size) {}

void FrameDecoder::reset() {
    buf_.clear();
    offset_ = 0;
}

DecodeResult FrameDecoder::consume(const std::uint8_t* data, std::size_t len,
                                   const FrameCallback& on_frame) {
    // 追加前惰性 compact：offset 大（消费多残留少）或占比过半时一次性搬移。
    if (offset_ > 0 && (offset_ >= kCompactThreshold || offset_ * 2 >= buf_.size())) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }
    if (len > 0 && data != nullptr) {
        buf_.insert(buf_.end(), data, data + len);
    }

    std::size_t i = offset_;
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
        // 零拷贝帧视图：直接指向内部缓冲，不拷贝（生命周期限于回调内）。
        on_frame(buf_.data() + i + kFrameHeaderSize, declared);
        i += needed;  // 粘包：游标推进，循环继续切下一帧。
    }

    if (oversized) {
        buf_.clear();  // 失同步后无法信任残流，清空交由调用方关闭连接。
        offset_ = 0;
        return DecodeResult::kOversized;
    }
    offset_ = i;  // 游标式消费：不立即 erase，留待惰性 compact。
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
