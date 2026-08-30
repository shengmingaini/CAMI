#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>
#include "mmo/protocol/codec/envelope_view.h"

namespace mmo { namespace protocol {

// 拥有底层表示并提供视图：
//  - FlatBuffers 路径：零拷贝借用输入缓冲（不分配），view 指向输入字节；
//  - Protobuf  路径：拥有解析出的 MessageEnvelope，view 指向其字段。
class OwnedEnvelope {
public:
  // FlatBuffers：零拷贝，借用 raw（调用方须保证 raw 在对象存活期间有效）
  static OwnedEnvelope MakeFbs(std::string_view raw, EnvelopeView view);
  // Protobuf：拥有 msg（按 MessageEnvelope* 删除）
  static OwnedEnvelope MakeProtobuf(std::unique_ptr<void, void(*)(void*)> msg, EnvelopeView view);

  const EnvelopeView& view() const noexcept { return view_; }
  const uint8_t* data() const noexcept { return raw_.empty() ? nullptr : reinterpret_cast<const uint8_t*>(raw_.data()); }
  size_t size() const noexcept { return raw_.size(); }
  // Protobuf 路径下可取底层消息；FlatBuffers 路径返回 nullptr
  const void* protobuf_message() const noexcept { return pb_ ? pb_.get() : nullptr; }

private:
  OwnedEnvelope() : pb_(nullptr, nullptr) {}
  std::string_view raw_;                              // fbs：借用输入缓冲
  std::unique_ptr<void, void(*)(void*)> pb_;          // protobuf：拥有消息
  EnvelopeView view_;
};

}} // namespace mmo::protocol
