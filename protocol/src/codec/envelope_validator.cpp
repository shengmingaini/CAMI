#include "mmo/protocol/codec/envelope_validator.h"
#include "mmo/core/error/error_code.h"

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

Result<void> EnvelopeValidator::Validate(const EnvelopeView& env, uint32_t expected_version) {
  if (env.message_type == EnvelopeMessageType::Unknown)
    return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "unknown message type"));

  if (env.source.empty())
    return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "empty source"));

  // 版本不匹配：返回 VERSION_CONFLICT，禁止静默降级
  if (env.version != expected_version)
    return Result<void>::Fail(Error(ErrorCode::VERSION_CONFLICT, "unsupported protocol version"));

  const bool needs_payload =
      env.message_type == EnvelopeMessageType::Command ||
      env.message_type == EnvelopeMessageType::Query   ||
      env.message_type == EnvelopeMessageType::Event;

  if (needs_payload && env.payload.empty())
    return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "empty payload for command/query/event"));

  if (env.payload.size() > kMaxPayloadBytes)
    return Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "payload exceeds MaxPayloadBytes"));

  return Result<void>::Ok();
}

}} // namespace mmo::protocol
