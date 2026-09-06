// server/gamenode/combat/src/skill/buff_def.cpp — TASK-021 §15.2
//
// 自带**受限 JSON 解析器**（对象 / 数组 / 字符串 / 数字 / 布尔 / null），
// 与 TASK-017 items / TASK-018 npc / TASK-019 quest / TASK-021 skill_registry
// 同一策略：不引入第三方依赖。遇到不支持的语法立即报错（§21 Forbidden 静默默认）。
//
// BuffRegistry 仅做定义加载与 buff_id 校验（实际施加在 TASK-023）：
// 技能若引用不存在的 buff，加载期即报错（§19 Failure）。

#include "mmo/game/combat/skill/buff_def.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::combat {
namespace {

using core::ErrorCode;
using core::Error;

// ---- 受限 JSON 值 ----------------------------------------------------------

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type{Type::Null};
    bool boolean{false};
    double number{0.0};
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Find(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& kv : obj) {
            if (std::string_view(kv.first) == key) return &kv.second;
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : s_(std::move(text)) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        if (p_ != s_.size()) return FailAt("trailing content after top-level value");
        return true;
    }
    const std::string& Error() const noexcept { return err_; }

private:
    void SkipWs() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p_;
            else break;
        }
    }
    bool FailAt(const char* msg) {
        err_ = std::string(msg) + " (offset " + std::to_string(p_) + ")";
        return false;
    }
    char Peek() const noexcept { return p_ < s_.size() ? s_[p_] : '\0'; }

    bool ParseValue(JsonValue& v) {
        switch (Peek()) {
            case '{': return ParseObject(v);
            case '[': return ParseArray(v);
            case '"': {
                v.type = JsonValue::Type::String;
                return ParseString(v.str);
            }
            case 't': case 'f': return ParseBool(v);
            case 'n': return ParseNull(v);
            default: return ParseNumber(v);
        }
    }
    bool ParseObject(JsonValue& v) {
        v.type = JsonValue::Type::Object;
        ++p_;
        SkipWs();
        if (Peek() == '}') { ++p_; return true; }
        for (;;) {
            SkipWs();
            if (Peek() != '"') return FailAt("expected object key");
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (Peek() != ':') return FailAt("expected ':' after object key");
            ++p_;
            SkipWs();
            JsonValue child;
            if (!ParseValue(child)) return false;
            v.obj.emplace_back(std::move(key), std::move(child));
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++p_; continue; }
            if (c == '}') { ++p_; return true; }
            return FailAt("expected ',' or '}' in object");
        }
    }
    bool ParseArray(JsonValue& v) {
        v.type = JsonValue::Type::Array;
        ++p_;
        SkipWs();
        if (Peek() == ']') { ++p_; return true; }
        for (;;) {
            SkipWs();
            JsonValue child;
            if (!ParseValue(child)) return false;
            v.arr.push_back(std::move(child));
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++p_; continue; }
            if (c == ']') { ++p_; return true; }
            return FailAt("expected ',' or ']' in array");
        }
    }
    bool ParseString(std::string& out) {
        if (Peek() != '"') return FailAt("expected string");
        ++p_;
        out.clear();
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= s_.size()) return FailAt("unterminated escape");
            const char e = s_[p_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (p_ + 4 > s_.size()) return FailAt("bad \\u escape");
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[p_ + static_cast<std::size_t>(i)];
                        unsigned d = 0;
                        if (h >= '0' && h <= '9') d = static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') d = static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') d = static_cast<unsigned>(h - 'A' + 10);
                        else return FailAt("bad hex digit in \\u escape");
                        code = code * 16 + d;
                    }
                    p_ += 4;
                    if (code < 0x80) out.push_back(static_cast<char>(code));
                    else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return FailAt("unsupported escape");
            }
        }
        return FailAt("unterminated string");
    }
    bool ParseBool(JsonValue& v) {
        if (s_.compare(p_, 4, "true") == 0) {
            v.type = JsonValue::Type::Bool; v.boolean = true; p_ += 4; return true;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            v.type = JsonValue::Type::Bool; v.boolean = false; p_ += 5; return true;
        }
        return FailAt("expected true/false");
    }
    bool ParseNull(JsonValue& v) {
        if (s_.compare(p_, 4, "null") != 0) return FailAt("expected null");
        v.type = JsonValue::Type::Null; p_ += 4; return true;
    }
    bool ParseNumber(JsonValue& v) {
        const std::size_t start = p_;
        if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
        bool digits = false;
        while (p_ < s_.size() &&
               (std::isdigit(static_cast<unsigned char>(s_[p_])) != 0 || s_[p_] == '.' ||
                s_[p_] == 'e' || s_[p_] == 'E' || s_[p_] == '-' || s_[p_] == '+')) {
            if (std::isdigit(static_cast<unsigned char>(s_[p_])) != 0) digits = true;
            ++p_;
        }
        if (!digits) return FailAt("expected number");
        v.type = JsonValue::Type::Number;
        v.number = std::stod(s_.substr(start, p_ - start));
        return true;
    }

    std::string s_;
    std::size_t p_{0};
    std::string err_;
};

bool UintField(const JsonValue& obj, std::string_view key, bool required,
               std::uint64_t fallback, std::uint64_t& out, std::string& err) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr) {
        if (required) { err = "missing required field '" + std::string(key) + "'"; return false; }
        out = fallback; return true;
    }
    if (v->type != JsonValue::Type::Number || v->number < 0) {
        err = "field '" + std::string(key) + "' must be a non-negative number";
        return false;
    }
    out = static_cast<std::uint64_t>(v->number);
    return true;
}

bool StrField(const JsonValue& obj, std::string_view key, bool required,
              std::string& out, std::string& err) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr) {
        if (required) { err = "missing required field '" + std::string(key) + "'"; return false; }
        return true;
    }
    if (v->type != JsonValue::Type::String) {
        err = "field '" + std::string(key) + "' must be a string";
        return false;
    }
    out = v->str;
    return true;
}

}  // namespace

core::Result<void> BuffRegistry::Load(const std::string& json_text) {
    JsonParser parser(json_text);
    JsonValue root;
    if (!parser.Parse(root)) {
        return core::Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT,
                  std::string("buff config parse error: ") + parser.Error()));
    }
    // 兼容两种格式：裸数组 [ ... ]，或 {"buffs":[ ... ]}（与 BuffSystem::LoadFromConfig 同源 buffs.json）。
    const JsonValue* arr = &root;
    if (root.type == JsonValue::Type::Object) {
        const JsonValue* b = root.Find("buffs");
        if (b == nullptr || b->type != JsonValue::Type::Array) {
            return core::Result<void>::Fail(Error(
                ErrorCode::INVALID_ARGUMENT,
                "buff config must be an array or an object with a 'buffs' array"));
        }
        arr = b;
    } else if (root.type != JsonValue::Type::Array) {
        return core::Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "buff config top-level must be an array"));
    }
    for (const JsonValue& item : arr->arr) {
        if (item.type != JsonValue::Type::Object) {
            return core::Result<void>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "buff entry must be an object"));
        }
        BuffDef def{};
        std::string err;
        std::uint64_t id = 0;
        if (!UintField(item, "id", true, 0, id, err))
            return core::Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, err));
        if (id == 0)
            return core::Result<void>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "buff id must be > 0"));
        def.id = static_cast<std::uint32_t>(id);
        if (!StrField(item, "name", true, def.name, err))
            return core::Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, err));
        std::uint64_t u = 0;
        if (!UintField(item, "duration_ms", true, 0, u, err))
            return core::Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, err));
        def.duration_ms = static_cast<std::uint32_t>(u);
        if (!UintField(item, "max_stacks", false, 1, u, err))
            return core::Result<void>::Fail(Error(ErrorCode::INVALID_ARGUMENT, err));
        if (u > 0xFFFF)
            return core::Result<void>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "max_stacks overflow"));
        def.max_stacks = static_cast<std::uint16_t>(u);

        if (buffs_.find(def.id) != buffs_.end()) {
            return core::Result<void>::Fail(Error(
                ErrorCode::INVALID_ARGUMENT, "duplicate buff id " + std::to_string(def.id)));
        }
        buffs_.emplace(def.id, def);
    }
    return core::Result<void>::Ok();
}

}  // namespace mmo::game::combat
