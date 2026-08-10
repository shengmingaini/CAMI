#include "gateway/codec/frame_encoder.h"

#include <cstring>

namespace cami {
namespace gateway {
namespace codec {

std::vector<std::uint8_t> encode_frame(const std::uint8_t* payload, std::size_t len) {
    std::vector<std::uint8_t> out(kFrameHeaderSize + len);
    // uint32 LE 长度前缀
    out[0] = static_cast<std::uint8_t>(len & 0xFF);
    out[1] = static_cast<std::uint8_t>((len >> 8) & 0xFF);
    out[2] = static_cast<std::uint8_t>((len >> 16) & 0xFF);
    out[3] = static_cast<std::uint8_t>((len >> 24) & 0xFF);
    if (len > 0 && payload != nullptr) {
        std::memcpy(out.data() + kFrameHeaderSize, payload, len);
    }
    return out;
}

}  // namespace codec
}  // namespace gateway
}  // namespace cami
