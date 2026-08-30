#pragma once
#include "mmo/core/error/result.h"
#include "mmo/protocol/codec/envelope_view.h"

namespace mmo { namespace protocol {
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::Result;

// 信封校验：必填字段、版本、payload 上限。
class EnvelopeValidator {
public:
  static constexpr uint32_t kMaxPayloadBytes = 1u << 20; // 1 MiB 默认上限（战斗帧另设）

  // expected_version：当前协议栈支持的最高版本；不匹配返回 VERSION_CONFLICT（不静默降级）
  static Result<void> Validate(const EnvelopeView& env, uint32_t expected_version);
};

}} // namespace mmo::protocol
