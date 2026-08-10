#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "gateway/codec/codec_types.h"

namespace cami {
namespace gateway {
namespace codec {

// 封包：将一条 payload（FlatBuffers 二进制）包成 [uint32 LE 长度][payload]。
// 纯函数，零外部依赖；不解析 payload 内容（透传）。
std::vector<std::uint8_t> encode_frame(const std::uint8_t* payload, std::size_t len);

inline std::vector<std::uint8_t> encode_frame(const std::vector<std::uint8_t>& payload) {
    return encode_frame(payload.data(), payload.size());
}

}  // namespace codec
}  // namespace gateway
}  // namespace cami
