#pragma once
#include "mmo/core/error/result.h"
#include "mmo/protocol/codec/envelope_view.h"
#include "mmo/protocol/codec/owned_envelope.h"
#include <string_view>
#include <vector>

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

// 编解码器统一接口。Codec 无状态、线程安全，可在任意线程使用。
class ICodec {
public:
  virtual ~ICodec() = default;
  virtual Result<std::vector<uint8_t>> Encode(const EnvelopeView& env) const = 0;
  virtual Result<OwnedEnvelope> Decode(std::string_view bytes) const = 0;
};

}} // namespace mmo::protocol
