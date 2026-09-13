/// TASK-034 · 极简 JSON 解析器实现（递归下降）。

#include "mmo/client/json.h"

#include <cstdlib>

namespace mmo { namespace client {

namespace {

struct Parser {
    std::string_view s;
    std::size_t pos = 0;

    core::Result<JsonValue> Fail(const char* msg) {
        return core::Result<JsonValue>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, msg, core::domain::kCore));
    }

    void SkipWs() {
        while (pos < s.size()) {
            const char c = s[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    core::Result<JsonValue> ParseValue() {
        SkipWs();
        if (pos >= s.size()) return Fail("JSON 意外结束");
        const char c = s[pos];
        switch (c) {
            case '{': return ParseObject();
            case '[': return ParseArray();
            case '"': { JsonValue v; v.type = JsonValue::Type::Str; return ParseString(v.str); }
            case 't': case 'f': return ParseBool();
            case 'n': return ParseNull();
            default:  return ParseNumber();
        }
    }

    core::Result<JsonValue> ParseObject() {
        JsonValue v; v.type = JsonValue::Type::Obj;
        ++pos; // {
        SkipWs();
        if (pos < s.size() && s[pos] == '}') { ++pos; return core::Result<JsonValue>::Ok(std::move(v)); }
        while (true) {
            SkipWs();
            if (pos >= s.size() || s[pos] != '"') return Fail("JSON 对象键应为字符串");
            JsonValue key; key.type = JsonValue::Type::Str;
            auto kr = ParseString(key.str);
            if (!kr.HasValue()) return kr;
            SkipWs();
            if (pos >= s.size() || s[pos] != ':') return Fail("JSON 缺少 ':'");
            ++pos;
            auto val = ParseValue();
            if (!val.HasValue()) return val;
            v.obj[std::move(key.str)] = std::move(val.Value());
            SkipWs();
            if (pos >= s.size()) return Fail("JSON 对象未闭合");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == '}') { ++pos; break; }
            return Fail("JSON 对象期望 ',' 或 '}'");
        }
        return core::Result<JsonValue>::Ok(std::move(v));
    }

    core::Result<JsonValue> ParseArray() {
        JsonValue v; v.type = JsonValue::Type::Arr;
        ++pos; // [
        SkipWs();
        if (pos < s.size() && s[pos] == ']') { ++pos; return core::Result<JsonValue>::Ok(std::move(v)); }
        while (true) {
            auto val = ParseValue();
            if (!val.HasValue()) return val;
            v.arr.push_back(std::move(val.Value()));
            SkipWs();
            if (pos >= s.size()) return Fail("JSON 数组未闭合");
            if (s[pos] == ',') { ++pos; continue; }
            if (s[pos] == ']') { ++pos; break; }
            return Fail("JSON 数组期望 ',' 或 ']'");
        }
        return core::Result<JsonValue>::Ok(std::move(v));
    }

    core::Result<JsonValue> ParseString(std::string& out) {
        ++pos; // opening quote
        std::string buf;
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') { out = buf; JsonValue r; r.type = JsonValue::Type::Str; r.str = buf; return core::Result<JsonValue>::Ok(std::move(r)); }
            if (c == '\\') {
                if (pos >= s.size()) return Fail("JSON 字符串转义截断");
                const char e = s[pos++];
                switch (e) {
                    case '"':  buf += '"';  break;
                    case '\\': buf += '\\'; break;
                    case '/':  buf += '/';  break;
                    case 'n':  buf += '\n'; break;
                    case 't':  buf += '\t'; break;
                    case 'r':  buf += '\r'; break;
                    case 'b':  buf += '\b'; break;
                    case 'f':  buf += '\f'; break;
                    case 'u': {
                        if (pos + 4 > s.size()) return Fail("JSON \\u 截断");
                        // 仅支持 BMP 单码点（够配置用）
                        unsigned long cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = s[pos++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<unsigned long>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned long>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned long>(h - 'A' + 10);
                            else return Fail("JSON \\u 非法十六进制");
                        }
                        buf += static_cast<char>(cp & 0xFF);
                        break;
                    }
                    default: return Fail("JSON 非法转义");
                }
            } else {
                buf += c;
            }
        }
        return Fail("JSON 字符串未闭合");
    }

    core::Result<JsonValue> ParseNumber() {
        const std::size_t start = pos;
        bool is_float = false;
        while (pos < s.size()) {
            const char c = s[pos];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
                if (c == '.') is_float = true;
                ++pos;
            } else if (c == 'e' || c == 'E') {
                is_float = true; ++pos;
            } else break;
        }
        if (pos == start) return Fail("JSON 非法数字");
        const std::string num_str(s.substr(start, pos - start));
        JsonValue v; v.type = JsonValue::Type::Num;
        v.num = std::strtod(num_str.c_str(), nullptr);
        (void)is_float;
        return core::Result<JsonValue>::Ok(std::move(v));
    }

    core::Result<JsonValue> ParseBool() {
        if (s.substr(pos, 4) == "true")  { pos += 4; JsonValue v; v.type=JsonValue::Type::Bool; v.b=true;  return core::Result<JsonValue>::Ok(std::move(v)); }
        if (s.substr(pos, 5) == "false") { pos += 5; JsonValue v; v.type=JsonValue::Type::Bool; v.b=false; return core::Result<JsonValue>::Ok(std::move(v)); }
        return Fail("JSON 非法字面量");
    }

    core::Result<JsonValue> ParseNull() {
        if (s.substr(pos, 4) == "null") { pos += 4; JsonValue v; v.type=JsonValue::Type::Null; return core::Result<JsonValue>::Ok(std::move(v)); }
        return Fail("JSON 非法字面量");
    }
};

}  // namespace

core::Result<JsonValue> JsonParse(std::string_view text) {
    Parser p{text, 0};
    auto r = p.ParseValue();
    if (!r.HasValue()) return r;
    p.SkipWs();
    if (p.pos != text.size()) return core::Result<JsonValue>::Fail(core::Error(
        core::ErrorCode::INVALID_ARGUMENT, "JSON 解析后存在多余字符", core::domain::kCore));
    return r;
}

}}  // namespace mmo::client
