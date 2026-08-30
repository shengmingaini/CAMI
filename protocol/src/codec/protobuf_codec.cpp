#include "mmo/protocol/codec/protobuf_codec.h"
#include "mmo/protocol/codec/envelope_validator.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "envelope.pb.h"

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

namespace {

EnvelopeMessageType ToView(mmo::protocol::MessageType t) noexcept {
  switch (t) {
    case mmo::protocol::MT_COMMAND:   return EnvelopeMessageType::Command;
    case mmo::protocol::MT_QUERY:     return EnvelopeMessageType::Query;
    case mmo::protocol::MT_EVENT:     return EnvelopeMessageType::Event;
    case mmo::protocol::MT_RESPONSE:  return EnvelopeMessageType::Response;
    case mmo::protocol::MT_HEARTBEAT: return EnvelopeMessageType::Heartbeat;
    default:                          return EnvelopeMessageType::Unknown;
  }
}

mmo::protocol::MessageType FromView(EnvelopeMessageType t) noexcept {
  switch (t) {
    case EnvelopeMessageType::Command:   return mmo::protocol::MT_COMMAND;
    case EnvelopeMessageType::Query:     return mmo::protocol::MT_QUERY;
    case EnvelopeMessageType::Event:     return mmo::protocol::MT_EVENT;
    case EnvelopeMessageType::Response:  return mmo::protocol::MT_RESPONSE;
    case EnvelopeMessageType::Heartbeat: return mmo::protocol::MT_HEARTBEAT;
    default:                             return mmo::protocol::MT_UNKNOWN;
  }
}

std::unique_ptr<void, void(*)(void*)> WrapMsg(mmo::protocol::MessageEnvelope* p) {
  return {p, [](void* x) { delete static_cast<mmo::protocol::MessageEnvelope*>(x); }};
}

} // namespace

Result<std::vector<uint8_t>> ProtobufCodec::Encode(const EnvelopeView& env) const {
  mmo::protocol::MessageEnvelope msg;
  msg.set_message_id(env.message_id);
  msg.set_message_type(FromView(env.message_type));
  msg.set_version(env.version);
  msg.set_source(env.source);
  msg.set_timestamp_ms(env.timestamp_ms);
  msg.set_trace_id(env.trace_id);
  msg.set_request_id(env.request_id);
  msg.set_payload(env.payload);
  msg.set_transaction_id(env.transaction_id);
  msg.set_idempotency_key(env.idempotency_key);

  std::vector<uint8_t> buf(msg.ByteSizeLong());
  if (!msg.SerializeToArray(buf.data(), static_cast<int>(buf.size())))
    return Result<std::vector<uint8_t>>::Fail(
        Error(ErrorCode::INTERNAL_ERROR, "serialize envelope failed"));
  return Result<std::vector<uint8_t>>::Ok(std::move(buf));
}

Result<OwnedEnvelope> ProtobufCodec::Decode(std::string_view bytes) const {
  auto msg = std::make_unique<mmo::protocol::MessageEnvelope>();
  if (!msg->ParseFromArray(bytes.data(), static_cast<int>(bytes.size())))
    return Result<OwnedEnvelope>::Fail(
        Error(ErrorCode::INVALID_ARGUMENT, "parse envelope failed"));

  EnvelopeView v;
  v.message_id = msg->message_id();
  v.message_type = ToView(msg->message_type());
  v.version = msg->version();
  v.source = msg->source();
  v.timestamp_ms = msg->timestamp_ms();
  v.trace_id = msg->trace_id();
  v.request_id = msg->request_id();
  v.payload = std::string_view(msg->payload().data(), msg->payload().size());
  v.transaction_id = msg->transaction_id();
  v.idempotency_key = msg->idempotency_key();

  return Result<OwnedEnvelope>::Ok(
      OwnedEnvelope::MakeProtobuf(WrapMsg(msg.release()), std::move(v)));
}

}} // namespace mmo::protocol
