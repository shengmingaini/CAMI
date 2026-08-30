#include "mmo/protocol/codec/owned_envelope.h"

namespace mmo { namespace protocol {

OwnedEnvelope OwnedEnvelope::MakeFbs(std::string_view raw, EnvelopeView view) {
  OwnedEnvelope e;
  e.raw_ = raw;
  e.view_ = std::move(view);
  return e;
}

OwnedEnvelope OwnedEnvelope::MakeProtobuf(std::unique_ptr<void, void(*)(void*)> msg, EnvelopeView view) {
  OwnedEnvelope e;
  e.pb_ = std::move(msg);
  e.view_ = std::move(view);
  return e;
}

}} // namespace mmo::protocol
