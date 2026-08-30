#include "mmo/protocol/codec/flatbuf_codec.h"
#include "mmo/protocol/codec/envelope_validator.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "flatbuffers/flatbuffers.h"
#include "envelope_transport_generated.h"

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

namespace {

// 生成枚举 TransportMessageType_<Value>（flatc 命名），与视图枚举一一映射
EnvelopeMessageType ToView(TransportMessageType t) noexcept {
  switch (t) {
    case TransportMessageType_Command:   return EnvelopeMessageType::Command;
    case TransportMessageType_Query:     return EnvelopeMessageType::Query;
    case TransportMessageType_Event:     return EnvelopeMessageType::Event;
    case TransportMessageType_Response:  return EnvelopeMessageType::Response;
    case TransportMessageType_Heartbeat: return EnvelopeMessageType::Heartbeat;
    default:                             return EnvelopeMessageType::Unknown;
  }
}

TransportMessageType FromView(EnvelopeMessageType t) noexcept {
  switch (t) {
    case EnvelopeMessageType::Command:   return TransportMessageType_Command;
    case EnvelopeMessageType::Query:     return TransportMessageType_Query;
    case EnvelopeMessageType::Event:     return TransportMessageType_Event;
    case EnvelopeMessageType::Response:  return TransportMessageType_Response;
    case EnvelopeMessageType::Heartbeat: return TransportMessageType_Heartbeat;
    default:                             return TransportMessageType_Unknown;
  }
}

} // namespace

Result<std::vector<uint8_t>> FlatbufCodec::Encode(const EnvelopeView& env) const {
  flatbuffers::FlatBufferBuilder fbb;

  flatbuffers::Offset<flatbuffers::Vector<uint8_t>> payload_off;
  const bool has_payload = !env.payload.empty();
  if (has_payload) {
    payload_off = fbb.CreateVector(reinterpret_cast<const uint8_t*>(env.payload.data()),
                                   env.payload.size());
  }
  auto source = fbb.CreateString(env.source.data(), env.source.size());
  auto trace  = fbb.CreateString(env.trace_id.data(), env.trace_id.size());
  auto tid    = fbb.CreateString(env.transaction_id.data(), env.transaction_id.size());
  auto ikey   = fbb.CreateString(env.idempotency_key.data(), env.idempotency_key.size());

  TransportEnvelopeBuilder eb(fbb);
  eb.add_message_id(env.message_id);
  eb.add_message_type(FromView(env.message_type));
  eb.add_version(env.version);
  eb.add_source(source);
  eb.add_timestamp_ms(env.timestamp_ms);
  eb.add_trace_id(trace);
  eb.add_request_id(env.request_id);
  if (has_payload) {
    eb.add_payload(payload_off);
  }
  eb.add_transaction_id(tid);
  eb.add_idempotency_key(ikey);
  auto off = eb.Finish();
  fbb.Finish(off, TransportEnvelopeIdentifier());

  const uint8_t* p = fbb.GetBufferPointer();
  return Result<std::vector<uint8_t>>::Ok(std::vector<uint8_t>(p, p + fbb.GetSize()));
}

Result<OwnedEnvelope> FlatbufCodec::Decode(std::string_view bytes) const {
  if (bytes.size() < sizeof(flatbuffers::uoffset_t))
    return Result<OwnedEnvelope>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "empty buffer"));

  auto* p = reinterpret_cast<const uint8_t*>(bytes.data());
  if (!flatbuffers::BufferHasIdentifier(p, TransportEnvelopeIdentifier()))
    return Result<OwnedEnvelope>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "bad file identifier"));

  flatbuffers::Verifier v(p, bytes.size());
  if (!VerifyTransportEnvelopeBuffer(v))
    return Result<OwnedEnvelope>::Fail(Error(ErrorCode::INVALID_ARGUMENT, "envelope verify failed"));

  // GetRoot 只读根偏移，本身不分配；以下全部字段访问都是零拷贝借用输入缓冲
  auto* env = flatbuffers::GetRoot<TransportEnvelope>(p);

  EnvelopeView view;
  view.message_id   = env->message_id();
  view.message_type = ToView(env->message_type());
  view.version      = env->version();
  if (auto* s = env->source()) view.source = std::string_view(s->data(), s->size());
  view.timestamp_ms = env->timestamp_ms();
  if (auto* t = env->trace_id()) view.trace_id = std::string_view(t->data(), t->size());
  view.request_id = env->request_id();
  if (auto* pl = env->payload()) {
    view.payload = std::string_view(reinterpret_cast<const char*>(pl->data()), pl->size());
  }
  if (auto* t = env->transaction_id()) view.transaction_id = std::string_view(t->data(), t->size());
  if (auto* k = env->idempotency_key()) view.idempotency_key = std::string_view(k->data(), k->size());

  // 零拷贝：view 直接指向输入缓冲 bytes，不分配
  return Result<OwnedEnvelope>::Ok(OwnedEnvelope::MakeFbs(bytes, std::move(view)));
}

}} // namespace mmo::protocol
