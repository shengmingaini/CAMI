#pragma once
#include <cstdint>
#include <string_view>
#include "mmo/protocol/message_type.h"

namespace mmo { namespace protocol {

// 解码后的零拷贝视图。所有 string_view 字段指向底层缓冲（FlatBuffers 零拷贝 /
// Protobuf 指向已解析消息），调用方必须保证底层表示在视图存活期间不被释放。
struct EnvelopeView {
  uint64_t          message_id = 0;
  EnvelopeMessageType message_type = EnvelopeMessageType::Unknown;
  uint32_t          version = 0;
  std::string_view  source;
  int64_t           timestamp_ms = 0;
  std::string_view  trace_id;
  uint64_t          request_id = 0;
  std::string_view  payload;            // 具体 Command / Query / Event 的序列化结果
  std::string_view  transaction_id;
  std::string_view  idempotency_key;
};

}} // namespace mmo::protocol
