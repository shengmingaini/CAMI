// server/gamenode/combat/src/skill/skill_registry.cpp — TASK-021 §15.2
//
// 自带**受限 JSON 解析器**（对象 / 数组 / 字符串 / 数字 / 布尔 / null），
// 与 TASK-017 items / TASK-018 npc / TASK-019 quest 同一策略：不引入第三方依赖，
// 只支持配置所需子集，遇到不支持的语法立即报错（§21 Forbidden 静默默认）。
//
// 红线：缺字段 / 类型错 / 引用不存在的 buff / 重复 id → INVALID_ARGUMENT 并携带字段名，
// 禁止用默认值静默启动。

#include "mmo/game/combat/skill/skill_registry.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
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

// ---- 字段提取 --------------------------------------------------------------

core::Result<void> Fail(ErrorCode code, const std::string& what) {
    return core::Result<void>::Fail(Error(code, what, core::domain::kCore));
}

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

bool NumField(const JsonValue& obj, std::string_view key, bool required,
              double fallback, double& out, std::string& err) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr) {
        if (required) { err = "missing required field '" + std::string(key) + "'"; return false; }
        out = fallback; return true;
    }
    if (v->type != JsonValue::Type::Number) {
        err = "field '" + std::string(key) + "' must be a number";
        return false;
    }
    out = v->number;
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

std::optional<TargetType> TargetTypeFromName(std::string_view name) {
    struct Map { std::string_view name; TargetType type; };
    static constexpr Map kMap[] = {
        {"Self", TargetType::Self},
        {"SingleTarget", TargetType::SingleTarget},
        {"AoeCircle", TargetType::AoeCircle},
        {"AoeCone", TargetType::AoeCone},
        {"Projectile", TargetType::Projectile},
    };
    for (const Map& m : kMap) if (m.name == name) return m.type;
    return std::nullopt;
}

std::optional<EffectType> EffectTypeFromName(std::string_view name) {
    struct Map { std::string_view name; EffectType type; };
    static constexpr Map kMap[] = {
        {"Damage", EffectType::Damage},
        {"Heal", EffectType::Heal},
        {"ApplyBuff", EffectType::ApplyBuff},
        {"SpawnProjectile", EffectType::SpawnProjectile},
    };
    for (const Map& m : kMap) if (m.name == name) return m.type;
    return std::nullopt;
}

bool ParseEffect(const JsonValue& item, EffectDef& def, const BuffRegistry& buffs,
                 std::string& err) {
    if (item.type != JsonValue::Type::Object) { err = "effect must be an object"; return false; }
    std::string type_name;
    if (!StrField(item, "type", true, type_name, err)) return false;
    auto opt = EffectTypeFromName(type_name);
    if (!opt.has_value()) { err = "unknown effect type '" + type_name + "'"; return false; }
    def.type = *opt;
    double d = 0;
    if (!NumField(item, "school", false, 0.0, d, err)) return false;
    def.school = static_cast<std::uint32_t>(d);
    if (!NumField(item, "base", false, 0.0, d, err)) return false;
    def.base = static_cast<float>(d);
    if (!NumField(item, "coeff", false, 0.0, d, err)) return false;
    def.coeff = static_cast<float>(d);
    if (const JsonValue* c = item.Find("can_crit")) {
        if (c->type != JsonValue::Type::Bool) { err = "can_crit must be bool"; return false; }
        def.can_crit = c->boolean;
    }
    std::uint64_t u = 0;
    if (!UintField(item, "buff_id", false, 0, u, err)) return false;
    def.buff_id = static_cast<std::uint32_t>(u);
    if (!UintField(item, "duration_ms", false, 0, u, err)) return false;
    def.duration_ms = static_cast<std::uint32_t>(u);
    if (!UintField(item, "stacks", false, 1, u, err)) return false;
    if (u > 0xFFFF) { err = "stacks overflow"; return false; }
    def.stacks = static_cast<std::uint16_t>(u);
    if (!NumField(item, "speed", false, 10.0, d, err)) return false;
    def.speed = static_cast<float>(d);
    if (!NumField(item, "radius", false, 1.5, d, err)) return false;
    def.radius = static_cast<float>(d);
    if (!NumField(item, "max_distance", false, 30.0, d, err)) return false;
    def.max_distance = static_cast<float>(d);

    // ApplyBuff 引用的 buff 必须存在（§15.2 校验）。
    if (def.type == EffectType::ApplyBuff && def.buff_id != 0 && !buffs.Contains(def.buff_id)) {
        err = "effect references unknown buff_id " + std::to_string(def.buff_id);
        return false;
    }
    return true;
}

bool ParseSkill(const JsonValue& item, SkillDef& def, const BuffRegistry& buffs, std::string& err) {
    if (item.type != JsonValue::Type::Object) { err = "skill entry must be an object"; return false; }
    std::uint64_t id = 0;
    if (!UintField(item, "id", true, 0, id, err)) return false;
    def.id = static_cast<SkillId>(id);

    if (!StrField(item, "name", true, def.name, err)) return false;

    std::string tt;
    if (!StrField(item, "target_type", true, tt, err)) return false;
    auto opt = TargetTypeFromName(tt);
    if (!opt.has_value()) { err = "skill " + std::to_string(def.id) + ": unknown target_type '" + tt + "'"; return false; }
    def.target_type = *opt;

    double d = 0;
    if (!NumField(item, "cast_time", false, 0.0, d, err)) return false;
    def.cast_time = static_cast<float>(d);
    if (!NumField(item, "cooldown", false, 1.5, d, err)) return false;
    def.cooldown = static_cast<float>(d);
    if (!NumField(item, "range", false, 5.0, d, err)) return false;
    def.range = static_cast<float>(d);
    if (!NumField(item, "radius", false, 0.0, d, err)) return false;
    def.radius = static_cast<float>(d);
    if (!NumField(item, "cone_deg", false, 90.0, d, err)) return false;
    def.cone_deg = static_cast<float>(d);
    std::uint64_t u = 0;
    if (!UintField(item, "mana_cost", false, 0, u, err)) return false;
    def.mana_cost = static_cast<std::int64_t>(u);
    if (!UintField(item, "hp_cost", false, 0, u, err)) return false;
    def.hp_cost = static_cast<std::int64_t>(u);
    if (const JsonValue* c = item.Find("interruptible")) {
        if (c->type != JsonValue::Type::Bool) { err = "interruptible must be bool"; return false; }
        def.interruptible = c->boolean;
    }
    if (!UintField(item, "required_level", false, 1, u, err)) return false;
    def.required_level = static_cast<std::uint32_t>(u);

    const JsonValue* effs = item.Find("effects");
    if (effs == nullptr) {
        err = "skill " + std::to_string(def.id) + ": missing 'effects' array";
        return false;
    }
    if (effs->type != JsonValue::Type::Array || effs->arr.empty()) {
        err = "skill " + std::to_string(def.id) + ": 'effects' must be a non-empty array";
        return false;
    }
    for (const JsonValue& e : effs->arr) {
        EffectDef ed{};
        if (!ParseEffect(e, ed, buffs, err)) return false;
        def.effects.push_back(ed);
    }
    return true;
}

std::string ReadFile(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

core::Result<void> SkillRegistry::Load(std::string_view json_text, const BuffRegistry& buffs) {
    // 原子加载：先清理既有状态，避免失败/重复加载残留旧技能导致「重复 id」误报。
    skills_.clear();
    id_to_index_.clear();
    return LoadAppend(json_text, buffs);
}

core::Result<void> SkillRegistry::LoadAppend(std::string_view json_text, const BuffRegistry& buffs) {
    JsonParser parser{std::string(json_text)};
    JsonValue root;
    if (!parser.Parse(root)) {
        return Fail(ErrorCode::INVALID_ARGUMENT, std::string("skill config parse error: ") + parser.Error());
    }
    if (root.type != JsonValue::Type::Array) {
        return Fail(ErrorCode::INVALID_ARGUMENT, "skill config top-level must be an array");
    }
    for (const JsonValue& item : root.arr) {
        SkillDef def{};
        std::string err;
        if (!ParseSkill(item, def, buffs, err)) {
            return Fail(ErrorCode::INVALID_ARGUMENT, err);
        }
        if (id_to_index_.find(def.id) != id_to_index_.end()) {
            return Fail(ErrorCode::INVALID_ARGUMENT,
                        "duplicate skill id " + std::to_string(def.id));
        }
        const std::uint32_t idx = static_cast<std::uint32_t>(skills_.size());
        def.index_ = idx;
        id_to_index_.emplace(def.id, idx);
        skills_.push_back(std::move(def));
    }
    return core::Result<void>::Ok();
}

core::Result<void> SkillRegistry::LoadFromDir(std::string_view dir, const BuffRegistry& buffs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::string(dir), ec)) {
        return Fail(ErrorCode::NOT_FOUND,
                    std::string("skill config dir not found: ") + std::string(dir));
    }
    std::vector<std::string> files;
    for (auto it = std::filesystem::directory_iterator(std::string(dir), ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    // 原子加载：整个目录视为一次加载，开始前清理既有状态。
    skills_.clear();
    id_to_index_.clear();
    for (const auto& f : files) {
        const std::string text = ReadFile(f);
        if (text.empty()) {
            return Fail(ErrorCode::INVALID_ARGUMENT,
                        std::string("skill config empty or unreadable: ") + f);
        }
        auto r = LoadAppend(text, buffs);
        if (!r) return core::Result<void>::Fail(r.Err());
    }
    return core::Result<void>::Ok();
}

const SkillDef* SkillRegistry::Find(SkillId id) const noexcept {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return nullptr;
    return &skills_[it->second];
}

std::uint32_t SkillRegistry::Index(SkillId id) const noexcept {
    auto it = id_to_index_.find(id);
    if (it == id_to_index_.end()) return static_cast<std::uint32_t>(skills_.size());
    return it->second;
}

}  // namespace mmo::game::combat
