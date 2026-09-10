// server/dataservice/include/mmo/data/redis/serialization.h
//
// TASK-027 · Record <-> Redis 值 序列化（§8 数据模型）。
//
// 采用自描述二进制格式（魔数 + 版本 + 更新时刻 + payload 长度 + payload），
// 不引入额外依赖、可在本模块内闭环编解码；损坏数据返回错误而非崩溃（§16）。

#pragma once

#include <string>
#include <string_view>

#include "mmo/core/error/result.h"
#include "mmo/data/record.h"

namespace mmo::data::redis {

/// 将 Record 编码为单个字符串值（二进制安全）。
std::string EncodeRecord(const Record& rec);

/// 解码；输入损坏 / 魔数不匹配 / 长度越界时返回 Error（禁止抛异常、禁止崩溃）。
core::Result<Record> DecodeRecord(std::string_view blob);

}  // namespace mmo::data::redis
