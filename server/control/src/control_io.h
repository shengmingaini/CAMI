// server/control/src/control_io.h — 内部序列化（不进 include/，下游不可见）
//
// 简单、确定性文本序列化，供「经 IDataStore 持久化」使用。
// 字段分隔符 0x1F（unit separator），记录分隔符 '\n'。addr 约定不含分隔符（IP:port）。
// 解析失败一律返回 Result::Fail(INTERNAL_ERROR)，绝不静默产出半截数据。

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mmo/control/control_service.h"
#include "mmo/core/error/result.h"

namespace mmo::control::detail {

/// 序列化整个节点表（聚合为单条记录，键 "control:nodes"）。
std::string SerializeNodes(const std::vector<ControlNodeInfo>& nodes);

/// 反序列化节点表；任一记录损坏即整体失败。
core::Result<std::vector<ControlNodeInfo>> DeserializeNodes(std::string_view text);

/// 序列化配置（version + snapshot），键 "control:config"。
std::string SerializeConfig(std::uint64_t version, std::string_view snapshot);

/// 反序列化配置；返回 (version, snapshot)。
core::Result<std::pair<std::uint64_t, std::string>> DeserializeConfig(std::string_view text);

}  // namespace mmo::control::detail
