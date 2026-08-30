#pragma once

#include <string>
#include <utility>
#include <vector>

#include "mmo/core/error/result.h"

namespace mmo::core::config_internal {

/// 扁平化后的配置项：key 为点号路径，value 为原始文本。
using FlatEntry = std::pair<std::string, std::string>;
using FlatMap = std::vector<FlatEntry>;

/// 极简 JSON 解析器（内部头，不进 PUBLIC 接口）。
///
/// 设计取舍：
///   - 目标只是读配置文件，因此不支持流式、不支持注释、不保留类型信息；
///     数字保留**原始文本**，避免 double 中途失真，类型判断交给 Get<T> 按需解析。
///   - 嵌套结构扁平化为点号路径，便于 Get("a.b.c") 直接取值：
///       对象 {"a":{"b":1}}   -> "a.b" = "1"
///       数组 {"p":[10,20]}   -> "p[0]" = "10", "p[1]" = "20"
///   - 递归深度上限 kMaxDepth，防恶意/畸形输入把栈打爆。
///   - 支持 \uXXXX 转义（含 UTF-16 代理对 -> UTF-8）。
///
/// 失败语义：任何语法错误返回 INVALID_ARGUMENT，并带出错字符偏移，方便定位。
Result<void> ParseJson(std::string_view text, FlatMap& out);

/// 递归深度上限（防御栈溢出）。
inline constexpr std::size_t kJsonMaxDepth = 64;

}  // namespace mmo::core::config_internal
