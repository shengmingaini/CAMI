// scripting/gameplay/src/gameplay_manifest.cpp — TASK-033 §15.2 配置加载
//
// 自带**受限 JSON 解析器**（对象 / 数组 / 字符串 / 数字 / 布尔 / null），
// 与 TASK-017 items / TASK-018 npc / TASK-019 quests 同一策略：不引入第三方依赖。
//
// 红线：缺字段 / 类型错 / 未知枚举名 / tick_hz > 1 → INVALID_ARGUMENT 并携带字段路径，
// 禁止静默默认（§19）。文件 IO 只发生在 LoadManifestFile（Cold Path，§11）。

#include "mmo/gameplay/gameplay_manifest.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace mmo::gameplay {
namespace {

constexpr std::string_view kDomain = core::domain::kLua;

core::Result<ScriptManifest> Fail(core::ErrorCode code, const std::string& what,
                                  std::string* error) {
    if (error != nullptr) {
        *error = what;
    }
    return core::Result<ScriptManifest>::Fail(core::Error(code, what, kDomain));
}

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
        for (const auto& kv : obj) {
            if (std::string_view(kv.first) == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string text) : s_(std::move(text)) {}

    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out)) {
            return false;
        }
        SkipWs();
        if (p_ != s_.size()) {
            return FailAt("trailing content after top-level value");
        }
        return true;
    }

    const std::string& Error() const noexcept { return err_; }

private:
    void SkipWs() {
        while (p_ < s_.size()) {
            const char c = s_[p_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++p_;
            } else {
                break;
            }
        }
    }
    bool FailAt(const char* msg) {
        err_ = std::string(msg) + " (offset " + std::to_string(p_) + ")";
        return false;
    }
    char Peek() const noexcept { return p_ < s_.size() ? s_[p_] : '\0'; }

    bool ParseValue(JsonValue& v) {
        switch (Peek()) {
            case '{':
                return ParseObject(v);
            case '[':
                return ParseArray(v);
            case '"':
                v.type = JsonValue::Type::String;
                return ParseString(v.str);
            case 't':
            case 'f':
                return ParseBool(v);
            case 'n':
                return ParseNull(v);
            default:
                return ParseNumber(v);
        }
    }

    bool ParseObject(JsonValue& v) {
        v.type = JsonValue::Type::Object;
        ++p_;  // '{'
        SkipWs();
        if (Peek() == '}') {
            ++p_;
            return true;
        }
        for (;;) {
            SkipWs();
            if (Peek() != '"') {
                return FailAt("expected object key");
            }
            std::string key;
            if (!ParseString(key)) {
                return false;
            }
            SkipWs();
            if (Peek() != ':') {
                return FailAt("expected ':' after object key");
            }
            ++p_;
            SkipWs();
            JsonValue child;
            if (!ParseValue(child)) {
                return false;
            }
            v.obj.emplace_back(std::move(key), std::move(child));
            SkipWs();
            const char c = Peek();
            if (c == ',') {
                ++p_;
                continue;
            }
            if (c == '}') {
                ++p_;
                return true;
            }
            return FailAt("expected ',' or '}' in object");
        }
    }

    bool ParseArray(JsonValue& v) {
        v.type = JsonValue::Type::Array;
        ++p_;  // '['
        SkipWs();
        if (Peek() == ']') {
            ++p_;
            return true;
        }
        for (;;) {
            SkipWs();
            JsonValue child;
            if (!ParseValue(child)) {
                return false;
            }
            v.arr.emplace_back(std::move(child));
            SkipWs();
            const char c = Peek();
            if (c == ',') {
                ++p_;
                continue;
            }
            if (c == ']') {
                ++p_;
                return true;
            }
            return FailAt("expected ',' or ']' in array");
        }
    }

    bool ParseString(std::string& out) {
        if (Peek() != '"') {
            return FailAt("expected string");
        }
        ++p_;
        out.clear();
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ >= s_.size()) {
                return FailAt("unterminated escape");
            }
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
                    // 配置只用到 ASCII 注释与 id，\uXXXX 仅支持 BMP 直通为 UTF-8。
                    if (p_ + 4 > s_.size()) {
                        return FailAt("bad \\u escape");
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = s_[p_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') {
                            code |= static_cast<unsigned>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            code |= static_cast<unsigned>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            code |= static_cast<unsigned>(h - 'A' + 10);
                        } else {
                            return FailAt("bad \\u hex digit");
                        }
                    }
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0u | (code >> 6)));
                        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                    } else {
                        out.push_back(static_cast<char>(0xE0u | (code >> 12)));
                        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
                        out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
                    }
                    break;
                }
                default:
                    return FailAt("unsupported escape");
            }
        }
        return FailAt("unterminated string");
    }

    bool ParseNumber(JsonValue& v) {
        const std::size_t start = p_;
        if (Peek() == '-' || Peek() == '+') {
            ++p_;
        }
        bool any = false;
        while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_])) != 0) {
            ++p_;
            any = true;
        }
        if (p_ < s_.size() && s_[p_] == '.') {
            ++p_;
            while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_])) != 0) {
                ++p_;
                any = true;
            }
        }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            ++p_;
            if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) {
                ++p_;
            }
            while (p_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[p_])) != 0) {
                ++p_;
            }
        }
        if (!any) {
            return FailAt("expected value");
        }
        v.type = JsonValue::Type::Number;
        v.number = std::strtod(s_.substr(start, p_ - start).c_str(), nullptr);
        return true;
    }

    bool ParseBool(JsonValue& v) {
        if (s_.compare(p_, 4, "true") == 0) {
            p_ += 4;
            v.type = JsonValue::Type::Bool;
            v.boolean = true;
            return true;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            p_ += 5;
            v.type = JsonValue::Type::Bool;
            v.boolean = false;
            return true;
        }
        return FailAt("expected 'true'/'false'");
    }

    bool ParseNull(JsonValue& v) {
        if (s_.compare(p_, 4, "null") != 0) {
            return FailAt("expected 'null'");
        }
        p_ += 4;
        v.type = JsonValue::Type::Null;
        return true;
    }

    std::string s_;
    std::size_t p_{0};
    std::string err_;
};

// ---- 字段读取助手（每条都带字段路径，便于定位配置错误）--------------------

bool FieldString(const JsonValue& obj, std::string_view key, std::string& out, bool required,
                 const std::string& path, std::string* error) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr || v->type == JsonValue::Type::Null) {
        if (!required) {
            return true;
        }
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 缺失（必填）";
        }
        return false;
    }
    if (v->type != JsonValue::Type::String) {
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 期望字符串";
        }
        return false;
    }
    out = v->str;
    return true;
}

bool FieldNumber(const JsonValue& obj, std::string_view key, double& out, bool required,
                 const std::string& path, std::string* error) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr || v->type == JsonValue::Type::Null) {
        if (!required) {
            return true;
        }
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 缺失（必填）";
        }
        return false;
    }
    if (v->type != JsonValue::Type::Number) {
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 期望数字";
        }
        return false;
    }
    out = v->number;
    return true;
}

bool FieldBool(const JsonValue& obj, std::string_view key, bool& out, bool required,
               const std::string& path, std::string* error) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr || v->type == JsonValue::Type::Null) {
        if (!required) {
            return true;
        }
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 缺失（必填）";
        }
        return false;
    }
    if (v->type != JsonValue::Type::Bool) {
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 期望布尔";
        }
        return false;
    }
    out = v->boolean;
    return true;
}

bool FieldStringArray(const JsonValue& obj, std::string_view key, std::vector<std::string>& out,
                      bool required, const std::string& path, std::string* error) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr || v->type == JsonValue::Type::Null) {
        if (!required) {
            return true;
        }
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 缺失（必填）";
        }
        return false;
    }
    if (v->type != JsonValue::Type::Array) {
        if (error != nullptr) {
            *error = path + "." + std::string(key) + ": 期望字符串数组";
        }
        return false;
    }
    out.clear();
    out.reserve(v->arr.size());
    for (std::size_t i = 0; i < v->arr.size(); ++i) {
        if (v->arr[i].type != JsonValue::Type::String) {
            if (error != nullptr) {
                *error = path + "." + std::string(key) + "[" + std::to_string(i) + "]: 期望字符串";
            }
            return false;
        }
        out.push_back(v->arr[i].str);
    }
    return true;
}

bool ParseOneBinding(const JsonValue& item, std::size_t index, ScriptBinding& out,
                     std::string* error) {
    const std::string path = "scripts[" + std::to_string(index) + "]";

    std::string category_name;
    if (!FieldString(item, "category", category_name, true, path, error)) {
        return false;
    }
    if (!ParseCategory(category_name, out.category)) {
        if (error != nullptr) {
            *error = path + ".category: 未知类别 '" + category_name +
                     "'（期望 quest/skill/ai/boss/event）";
        }
        return false;
    }

    std::vector<std::string> hook_names;
    if (!FieldStringArray(item, "hooks", hook_names, true, path, error)) {
        return false;
    }
    out.hooks.clear();
    for (const std::string& h : hook_names) {
        Hook parsed{};
        if (!ParseHook(h, parsed)) {
            if (error != nullptr) {
                *error = path + ".hooks: 未知 hook '" + h + "'";
            }
            return false;
        }
        out.hooks.push_back(parsed);
    }

    if (!FieldString(item, "name", out.name, true, path, error)) {
        return false;
    }
    if (!FieldString(item, "path", out.path, true, path, error)) {
        return false;
    }
    if (!FieldStringArray(item, "keys", out.keys, false, path, error)) {
        return false;
    }
    if (!FieldStringArray(item, "requires", out.requires_refs, false, path, error)) {
        return false;
    }
    if (!FieldNumber(item, "tick_hz", out.tick_hz, false, path, error)) {
        return false;
    }
    if (!FieldBool(item, "enabled", out.enabled, false, path, error)) {
        return false;
    }
    return true;
}

}  // namespace

// ===========================================================================
// 枚举 / 名称映射
// ===========================================================================

const char* ToString(ScriptCategory c) noexcept {
    switch (c) {
        case ScriptCategory::Quest: return "quest";
        case ScriptCategory::Skill: return "skill";
        case ScriptCategory::Ai: return "ai";
        case ScriptCategory::Boss: return "boss";
        case ScriptCategory::Event: return "event";
    }
    return "UNKNOWN";
}

bool ParseCategory(std::string_view text, ScriptCategory& out) noexcept {
    if (text == "quest") {
        out = ScriptCategory::Quest;
    } else if (text == "skill") {
        out = ScriptCategory::Skill;
    } else if (text == "ai") {
        out = ScriptCategory::Ai;
    } else if (text == "boss") {
        out = ScriptCategory::Boss;
    } else if (text == "event") {
        out = ScriptCategory::Event;
    } else {
        return false;
    }
    return true;
}

const char* ToString(Hook h) noexcept {
    switch (h) {
        case Hook::SkillFormula: return "skill_formula";
        case Hook::QuestEvent: return "quest_event";
        case Hook::AiDecide: return "ai_decide";
        case Hook::BossPhase: return "boss_phase";
        case Hook::ActivityModifier: return "activity_modifier";
    }
    return "UNKNOWN";
}

bool ParseHook(std::string_view text, Hook& out) noexcept {
    if (text == "skill_formula") {
        out = Hook::SkillFormula;
    } else if (text == "quest_event") {
        out = Hook::QuestEvent;
    } else if (text == "ai_decide") {
        out = Hook::AiDecide;
    } else if (text == "boss_phase") {
        out = Hook::BossPhase;
    } else if (text == "activity_modifier") {
        out = Hook::ActivityModifier;
    } else {
        return false;
    }
    return true;
}

std::string_view HookEventName(Hook h) noexcept {
    // 脚本侧契约名（§7「C++ 只认这四个函数」的 name 参数取值）。
    // 与 Hook 的调试名相同，但是**独立常量**：改名 Hook 不应隐式改变脚本侧契约。
    return ToString(h);
}

std::size_t ScriptManifest::EnabledCount() const noexcept {
    std::size_t n = 0;
    for (const ScriptBinding& b : scripts) {
        if (b.enabled) {
            ++n;
        }
    }
    return n;
}

// ===========================================================================
// 载荷容器
// ===========================================================================

void GameplayPayload::Reset() noexcept {
    name_[0] = '\0';
    name_len_ = 0;
    field_count_ = 0;
}

bool GameplayPayload::SetName(std::string_view v) noexcept {
    if (v.size() > kNameCap) {
        return false;
    }
    for (std::size_t i = 0; i < v.size(); ++i) {
        name_[i] = v[i];
    }
    name_len_ = static_cast<std::uint8_t>(v.size());
    return true;
}

bool GameplayPayload::Add(ScalarField&& f, std::string_view key) noexcept {
    if (field_count_ >= kMaxFields) {
        return false;
    }
    if (key.empty() || key.size() > ScalarField::kNameCap) {
        return false;
    }
    for (std::size_t i = 0; i < key.size(); ++i) {
        f.name[i] = key[i];
    }
    f.name_len = static_cast<std::uint8_t>(key.size());
    fields_[field_count_] = f;
    ++field_count_;
    return true;
}

bool GameplayPayload::AddInt(std::string_view key, std::int64_t v) noexcept {
    ScalarField f{};
    f.kind = FieldKind::Integer;
    f.i = v;
    return Add(std::move(f), key);
}

bool GameplayPayload::AddNum(std::string_view key, double v) noexcept {
    ScalarField f{};
    f.kind = FieldKind::Number;
    f.n = v;
    return Add(std::move(f), key);
}

bool GameplayPayload::AddBool(std::string_view key, bool v) noexcept {
    ScalarField f{};
    f.kind = FieldKind::Bool;
    f.b = v;
    return Add(std::move(f), key);
}

bool GameplayPayload::AddStr(std::string_view key, std::string_view v) noexcept {
    if (v.size() > ScalarField::kTextCap) {
        return false;
    }
    ScalarField f{};
    f.kind = FieldKind::String;
    for (std::size_t i = 0; i < v.size(); ++i) {
        f.text[i] = v[i];
    }
    f.text_len = static_cast<std::uint8_t>(v.size());
    return Add(std::move(f), key);
}

const ScalarField* GameplayPayload::Field(std::size_t i) const noexcept {
    return i < field_count_ ? &fields_[i] : nullptr;
}

std::int64_t GameplayPayload::GetInt(std::string_view key, std::int64_t dflt) const noexcept {
    for (std::size_t i = 0; i < field_count_; ++i) {
        const ScalarField& f = fields_[i];
        if (f.Name() != key) {
            continue;
        }
        if (f.kind == FieldKind::Integer) {
            return f.i;
        }
        if (f.kind == FieldKind::Number) {
            return static_cast<std::int64_t>(f.n);
        }
        if (f.kind == FieldKind::Bool) {
            return f.b ? 1 : 0;
        }
        return dflt;
    }
    return dflt;
}

double GameplayPayload::GetNum(std::string_view key, double dflt) const noexcept {
    for (std::size_t i = 0; i < field_count_; ++i) {
        const ScalarField& f = fields_[i];
        if (f.Name() != key) {
            continue;
        }
        if (f.kind == FieldKind::Number) {
            return f.n;
        }
        if (f.kind == FieldKind::Integer) {
            return static_cast<double>(f.i);
        }
        return dflt;
    }
    return dflt;
}

bool GameplayPayload::GetBool(std::string_view key, bool dflt) const noexcept {
    for (std::size_t i = 0; i < field_count_; ++i) {
        const ScalarField& f = fields_[i];
        if (f.Name() != key) {
            continue;
        }
        if (f.kind == FieldKind::Bool) {
            return f.b;
        }
        if (f.kind == FieldKind::Integer) {
            return f.i != 0;
        }
        return dflt;
    }
    return dflt;
}

std::string_view GameplayPayload::GetStr(std::string_view key, std::string_view dflt) const noexcept {
    for (std::size_t i = 0; i < field_count_; ++i) {
        const ScalarField& f = fields_[i];
        if (f.Name() == key && f.kind == FieldKind::String) {
            return f.Text();
        }
    }
    return dflt;
}

// ===========================================================================
// 钳制（§19）
// ===========================================================================

bool ClampFormulaAmount(double raw, double& out, const char*& reason) noexcept {
    if (!std::isfinite(raw)) {
        out = 0.0;
        reason = "formula: non-finite (NaN/Inf)";
        return false;
    }
    if (raw < 0.0) {
        out = 0.0;
        reason = "formula: negative amount";
        return false;
    }
    if (raw > kMaxFormulaAmount) {
        out = kMaxFormulaAmount;
        reason = "formula: above max_raw_damage";
        return false;
    }
    out = raw;
    reason = nullptr;
    return true;
}

bool ClampMultiplier(double raw, double& out) noexcept {
    if (!std::isfinite(raw) || raw < 0.0) {
        out = 0.0;
        return false;
    }
    if (raw > kMaxMultiplier) {
        out = kMaxMultiplier;
        return false;
    }
    out = raw;
    return true;
}

AiAction ClampAiAction(std::int64_t raw, bool& clamped) noexcept {
    clamped = false;
    const auto count = static_cast<std::int64_t>(AiAction::kCount);
    if (raw < 0) {
        clamped = true;
        return AiAction::Idle;
    }
    if (raw >= count) {
        clamped = true;
        return AiAction::Idle;
    }
    return static_cast<AiAction>(raw);
}

std::int64_t ClampBossPhase(std::int64_t raw, bool& clamped) noexcept {
    clamped = false;
    if (raw < 1) {
        clamped = true;
        return 1;
    }
    if (raw > kMaxBossPhase) {
        clamped = true;
        return kMaxBossPhase;
    }
    return raw;
}

// ===========================================================================
// 解析 / 校验 / 序列化
// ===========================================================================

core::Result<ScriptManifest> ParseManifest(std::string_view text, std::string* error) {
    JsonValue root;
    // 【踩坑】必须用花括号：`JsonParser parser(std::string(text));` 会被解析成
    // 函数声明（most vexing parse）→ 报 `request for member 'Parse' in ... non-class type`。
    JsonParser parser{std::string(text)};
    if (!parser.Parse(root)) {
        return Fail(core::ErrorCode::INVALID_ARGUMENT,
                    "scripts.json 解析失败：" + parser.Error(), error);
    }
    if (root.type != JsonValue::Type::Object) {
        return Fail(core::ErrorCode::INVALID_ARGUMENT, "scripts.json 顶层必须是对象", error);
    }

    ScriptManifest manifest;

    double version = 1.0;
    if (!FieldNumber(root, "version", version, false, "root", error)) {
        return Fail(core::ErrorCode::INVALID_ARGUMENT, error != nullptr ? *error : "", error);
    }
    manifest.version = static_cast<std::uint32_t>(version);

    const JsonValue* list = root.Find("scripts");
    if (list == nullptr || list->type != JsonValue::Type::Array) {
        return Fail(core::ErrorCode::INVALID_ARGUMENT, "scripts.json 缺少 scripts 数组", error);
    }
    manifest.scripts.reserve(list->arr.size());
    for (std::size_t i = 0; i < list->arr.size(); ++i) {
        if (list->arr[i].type != JsonValue::Type::Object) {
            return Fail(core::ErrorCode::INVALID_ARGUMENT,
                        "scripts[" + std::to_string(i) + "]: 期望对象", error);
        }
        ScriptBinding binding;
        if (!ParseOneBinding(list->arr[i], i, binding, error)) {
            const std::string detail = error != nullptr ? *error : std::string("字段错误");
            return Fail(core::ErrorCode::INVALID_ARGUMENT,
                        detail.empty() ? "scripts[" + std::to_string(i) + "]: 字段错误" : detail,
                        error);
        }
        manifest.scripts.push_back(std::move(binding));
    }
    return core::Result<ScriptManifest>::Ok(std::move(manifest));
}

core::Result<ScriptManifest> LoadManifestFile(std::string_view path, std::string* error) {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return Fail(core::ErrorCode::NOT_FOUND,
                    "无法打开脚本清单：" + std::string(path), error);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return ParseManifest(buffer.str(), error);
}

std::vector<std::string> ValidateManifest(const ScriptManifest& manifest) {
    std::vector<std::string> issues;

    // 类别 ↔ hook 的允许组合（配置错误必须在编译任何脚本之前暴露）。
    const auto hook_allowed = [](ScriptCategory c, Hook h) noexcept {
        switch (c) {
            case ScriptCategory::Quest: return h == Hook::QuestEvent;
            case ScriptCategory::Skill: return h == Hook::SkillFormula;
            case ScriptCategory::Ai: return h == Hook::AiDecide;
            case ScriptCategory::Boss: return h == Hook::BossPhase;
            case ScriptCategory::Event: return h == Hook::ActivityModifier;
        }
        return false;
    };

    for (std::size_t i = 0; i < manifest.scripts.size(); ++i) {
        const ScriptBinding& b = manifest.scripts[i];
        const std::string where = "scripts[" + std::to_string(i) + "]('" + b.name + "')";

        if (b.name.empty()) {
            issues.push_back(where + ": name 为空");
        }
        if (b.path.empty()) {
            issues.push_back(where + ": path 为空");
        }
        if (b.hooks.empty()) {
            issues.push_back(where + ": 未挂任何 hook");
        }
        if (b.tick_hz < 0.0 || b.tick_hz > 1.0) {
            // §21「禁止高频（> 1Hz）调用周期脚本」——配置层直接拦下，不给运行期留机会。
            issues.push_back(where + ": tick_hz=" + std::to_string(b.tick_hz) +
                             " 越界（仅允许 0 或 1）");
        }
        for (Hook h : b.hooks) {
            if (!hook_allowed(b.category, h)) {
                issues.push_back(where + ": category=" + ToString(b.category) +
                                 " 与 hook=" + ToString(h) + " 不匹配");
            }
        }
        for (const std::string& r : b.requires_refs) {
            if (r.find(':') == std::string::npos) {
                issues.push_back(where + ": requires '" + r + "' 缺少 '<ns>:<id>' 形式");
            }
        }

        for (std::size_t j = i + 1; j < manifest.scripts.size(); ++j) {
            if (!b.name.empty() && b.name == manifest.scripts[j].name) {
                issues.push_back(where + ": 与 scripts[" + std::to_string(j) + "] 重名");
            }
        }
    }
    return issues;
}

std::string ToJson(const ScriptManifest& manifest) {
    std::ostringstream out;
    out << "{\n  \"version\": " << manifest.version << ",\n  \"scripts\": [\n";
    for (std::size_t i = 0; i < manifest.scripts.size(); ++i) {
        const ScriptBinding& b = manifest.scripts[i];
        out << "    { \"name\": \"" << b.name << "\", \"path\": \"" << b.path
            << "\", \"category\": \"" << ToString(b.category) << "\", \"hooks\": [";
        for (std::size_t k = 0; k < b.hooks.size(); ++k) {
            out << (k == 0 ? "" : ", ") << '"' << ToString(b.hooks[k]) << '"';
        }
        out << "], \"keys\": [";
        for (std::size_t k = 0; k < b.keys.size(); ++k) {
            out << (k == 0 ? "" : ", ") << '"' << b.keys[k] << '"';
        }
        out << "], \"requires\": [";
        for (std::size_t k = 0; k < b.requires_refs.size(); ++k) {
            out << (k == 0 ? "" : ", ") << '"' << b.requires_refs[k] << '"';
        }
        out << "], \"tick_hz\": " << b.tick_hz << ", \"enabled\": " << (b.enabled ? "true" : "false")
            << " }" << (i + 1 == manifest.scripts.size() ? "" : ",") << "\n";
    }
    out << "  ]\n}\n";
    return out.str();
}

}  // namespace mmo::gameplay
