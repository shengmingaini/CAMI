#pragma once

/// TASK-034 · 极简 JSON 解析器（仅覆盖客户端配置所需子集）。
///
/// 支持：对象 / 数组 / 字符串 / 数字（整数与浮点）/ true / false / null /
///      空白与标准转义。不追求完整 RFC 8259，但足以解析 config/client/client.json。

#include "mmo/core/error/result.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mmo { namespace client {

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;

    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Type::Null;

    bool        b = false;
    double      num = 0.0;
    std::string str;
    Array       arr;
    Object      obj;

    bool IsObject() const { return type == Type::Obj; }
    bool IsNull()   const { return type == Type::Null; }

    const JsonValue* Find(std::string_view key) const {
        if (type != Type::Obj) return nullptr;
        auto it = obj.find(std::string(key));
        return it == obj.end() ? nullptr : &it->second;
    }
    // 便捷取值（缺省 / 类型不符时回退）
    bool        AsBool(bool fb = false)        const { return type == Type::Bool ? b : fb; }
    double      AsNum(double fb = 0.0)         const { return type == Type::Num ? num : fb; }
    std::int64_t AsInt(std::int64_t fb = 0)    const { return type == Type::Num ? static_cast<std::int64_t>(num) : fb; }
    std::string AsStr(std::string_view fb = "") const { return type == Type::Str ? str : std::string(fb); }
};

/// 解析整段 JSON 文本。失败返回带路径信息的 Error。
core::Result<JsonValue> JsonParse(std::string_view text);

}}  // namespace mmo::client
