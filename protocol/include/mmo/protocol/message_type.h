#pragma once
#include <cstdint>

namespace mmo { namespace protocol {

// 信封消息类型（视图层枚举，与 Protobuf 的 mmo.protocol.MessageType /
// FlatBuffers 全局 MessageType 解耦，避免三套同名枚举在同一命名空间冲突）
enum class EnvelopeMessageType : uint8_t {
  Unknown = 0,
  Command = 1,
  Query = 2,
  Event = 3,
  Response = 4,
  Heartbeat = 5,
};

inline const char* ToString(EnvelopeMessageType t) noexcept {
  switch (t) {
    case EnvelopeMessageType::Command:   return "Command";
    case EnvelopeMessageType::Query:     return "Query";
    case EnvelopeMessageType::Event:     return "Event";
    case EnvelopeMessageType::Response:  return "Response";
    case EnvelopeMessageType::Heartbeat: return "Heartbeat";
    default:                             return "Unknown";
  }
}

}} // namespace mmo::protocol
