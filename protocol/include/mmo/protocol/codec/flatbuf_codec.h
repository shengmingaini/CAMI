#pragma once
#include "mmo/protocol/codec/icodec.h"

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

// FlatBuffers 编解码器（高频：移动 / AOI / 战斗）。
// Decode 零拷贝：直接指向输入缓冲，不分配；Encode 分配 FlatBufferBuilder 缓冲（允许）。
class FlatbufCodec : public ICodec {
public:
  Result<std::vector<uint8_t>> Encode(const EnvelopeView& env) const override;
  Result<OwnedEnvelope> Decode(std::string_view bytes) const override;
};

}} // namespace mmo::protocol
