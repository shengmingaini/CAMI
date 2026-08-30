#pragma once
#include "mmo/protocol/codec/icodec.h"

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

// Protobuf 编解码器（低频：管理面 / 数据面）。
class ProtobufCodec : public ICodec {
public:
  Result<std::vector<uint8_t>> Encode(const EnvelopeView& env) const override;
  Result<OwnedEnvelope> Decode(std::string_view bytes) const override;
};

}} // namespace mmo::protocol
