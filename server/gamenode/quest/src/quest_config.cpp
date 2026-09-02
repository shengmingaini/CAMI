// server/gamenode/quest/src/quest_config.cpp — TASK-019 §15.1 配置加载
//
// 自带**受限 JSON 解析器**（对象 / 数组 / 字符串 / 数字 / 布尔 / null），
// 与 TASK-017 items、TASK-018 npc 配置同一策略：不引入第三方依赖，
// 只支持配置所需的子集，遇到不支持的语法立即报错而非静默跳过。
//
// 红线：缺字段 / 类型错 / 目标数越界 → INVALID_ARGUMENT 并携带字段名，
// 禁止用默认值静默启动（同 TASK-016 exp_curve 的 ExpCurve() = delete 精神）。

#include "mmo/game/quest/quest_config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mmo::game::quest {
namespace {

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
            case '{': return ParseObject(v);
            case '[': return ParseArray(v);
            case '"': {
                v.type = JsonValue::Type::String;
                return ParseString(v.str);
            }
            case 't':
            case 'f': return ParseBool(v);
            case 'n': return ParseNull(v);
            default: return ParseNumber(v);
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
            if (!ParseValue(child)) return false;
            v.arr.push_back(std::move(child));
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
        if (Peek() != '"') return FailAt("expected string");
        ++p_;
        out.clear();
        while (p_ < s_.size()) {
            const char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
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
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
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
            v.type = JsonValue::Type::Bool;
            v.boolean = true;
            p_ += 4;
            return true;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            v.type = JsonValue::Type::Bool;
            v.boolean = false;
            p_ += 5;
            return true;
        }
        return FailAt("expected true/false");
    }

    bool ParseNull(JsonValue& v) {
        if (s_.compare(p_, 4, "null") != 0) return FailAt("expected null");
        v.type = JsonValue::Type::Null;
        p_ += 4;
        return true;
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

using mmo::core::ErrorCode;
using mmo::core::Error;

core::Result<std::vector<QuestDef>> FailVec(ErrorCode code, const std::string& what) {
    return core::Result<std::vector<QuestDef>>::Fail(
        Error(code, what, mmo::core::domain::kCore));
}

/// 取无符号整数字段（required=true 时缺失即报错）。
bool UintField(const JsonValue& obj, std::string_view key, bool required,
               std::uint64_t fallback, std::uint64_t& out, std::string& err) {
    const JsonValue* v = obj.Find(key);
    if (v == nullptr) {
        if (required) {
            err = std::string("missing required field '") + std::string(key) + "'";
            return false;
        }
        out = fallback;
        return true;
    }
    if (v->type != JsonValue::Type::Number || v->number < 0) {
        err = std::string("field '") + std::string(key) + "' must be a non-negative number";
        return false;
    }
    out = static_cast<std::uint64_t>(v->number);
    return true;
}

std::optional<ObjectiveType> ObjectiveTypeFromName(std::string_view name) {
    struct Map {
        std::string_view name;
        ObjectiveType type;
    };
    static constexpr Map kMap[] = {
        {"KillMonster", ObjectiveType::KillMonster},
        {"CollectItem", ObjectiveType::CollectItem},
        {"TalkNpc", ObjectiveType::TalkNpc},
        {"ReachLocation", ObjectiveType::ReachLocation},
        {"UseItem", ObjectiveType::UseItem},
    };
    for (const Map& m : kMap) {
        if (m.name == name) return m.type;
    }
    return std::nullopt;
}

/// 解析单条任务定义。目标数量字段名为 `count`（与配置保持一致），存入 required_count。
bool ParseQuest(const JsonValue& item, QuestDef& def, std::string& err) {
    if (item.type != JsonValue::Type::Object) {
        err = "quest entry must be an object";
        return false;
    }
    std::uint64_t id = 0;
    if (!UintField(item, "id", true, 0, id, err)) return false;
    def.id = static_cast<QuestId>(id);

    const JsonValue* title = item.Find("title");
    if (title == nullptr || title->type != JsonValue::Type::String) {
        err = "quest " + std::to_string(def.id) + ": field 'title' must be a string";
        return false;
    }
    def.title = title->str;

    std::uint64_t level = 1;
    if (!UintField(item, "required_level", false, 1, level, err)) return false;
    def.required_level = static_cast<std::uint32_t>(level);

    if (const JsonValue* pre = item.Find("prerequisites")) {
        if (pre->type != JsonValue::Type::Array) {
            err = "quest " + std::to_string(def.id) + ": 'prerequisites' must be an array";
            return false;
        }
        for (const JsonValue& p : pre->arr) {
            if (p.type != JsonValue::Type::Number) {
                err = "quest " + std::to_string(def.id) + ": prerequisite must be a number";
                return false;
            }
            def.prerequisites.push_back(static_cast<QuestId>(p.number));
        }
    }

    const JsonValue* objs = item.Find("objectives");
    if (objs == nullptr || objs->type != JsonValue::Type::Array || objs->arr.empty()) {
        err = "quest " + std::to_string(def.id) + ": 'objectives' must be a non-empty array";
        return false;
    }
    for (const JsonValue& o : objs->arr) {
        if (o.type != JsonValue::Type::Object) {
            err = "quest " + std::to_string(def.id) + ": objective must be an object";
            return false;
        }
        const JsonValue* type = o.Find("type");
        if (type == nullptr || type->type != JsonValue::Type::String) {
            err = "quest " + std::to_string(def.id) + ": objective 'type' must be a string";
            return false;
        }
        auto opt = ObjectiveTypeFromName(type->str);
        if (!opt.has_value()) {
            err = "quest " + std::to_string(def.id) + ": unknown objective type '" + type->str + "'";
            return false;
        }
        std::uint64_t target_id = 0;
        if (!UintField(o, "target_id", true, 0, target_id, err)) return false;
        // 目标数量字段名 `count`（与配置文件一致）；缺失即报错，禁止静默默认。
        std::uint64_t required = 1;
        if (!UintField(o, "count", true, 1, required, err)) return false;
        if (required == 0) {
            err = "quest " + std::to_string(def.id) + ": objective 'count' must be >= 1";
            return false;
        }
        ObjectiveDef od{};
        od.type = *opt;
        od.target_id = static_cast<std::uint32_t>(target_id);
        od.required_count = static_cast<std::uint32_t>(required);
        def.objectives.push_back(od);
    }

    std::uint64_t exp = 0;
    if (!UintField(item, "exp_reward", false, 0, exp, err)) return false;
    def.exp_reward = exp;
    std::uint64_t currency = 0;
    if (!UintField(item, "currency_reward", false, 0, currency, err)) return false;
    def.currency_reward = currency;

    if (const JsonValue* items = item.Find("item_rewards")) {
        if (items->type != JsonValue::Type::Array) {
            err = "quest " + std::to_string(def.id) + ": 'item_rewards' must be an array";
            return false;
        }
        for (const JsonValue& it : items->arr) {
            if (it.type != JsonValue::Type::Number) {
                err = "quest " + std::to_string(def.id) + ": item reward must be a number";
                return false;
            }
            def.item_rewards.push_back(static_cast<ItemId>(it.number));
        }
    }
    return true;
}

}  // namespace

// ---- QuestConfig（自由函数，配置化加载，禁止硬编码） ------------------------

core::Result<std::vector<QuestDef>> LoadQuestDefsFromFile(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        return FailVec(ErrorCode::NOT_FOUND,
                       std::string("quest config open failed: ") + std::string(path));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    JsonParser parser(text);
    JsonValue root;
    if (!parser.Parse(root)) {
        return FailVec(ErrorCode::INVALID_ARGUMENT,
                       std::string(path) + ": " + parser.Error());
    }

    const JsonValue* arr = (root.type == JsonValue::Type::Array) ? &root : root.Find("quests");
    if (arr == nullptr || arr->type != JsonValue::Type::Array) {
        return FailVec(ErrorCode::INVALID_ARGUMENT,
                       std::string(path) + ": missing 'quests' array");
    }

    std::vector<QuestDef> out;
    for (const JsonValue& item : arr->arr) {
        QuestDef def{};
        std::string err;
        if (!ParseQuest(item, def, err)) {
            return FailVec(ErrorCode::INVALID_ARGUMENT, std::string(path) + ": " + err);
        }
        out.push_back(std::move(def));
    }
    return core::Result<std::vector<QuestDef>>::Ok(std::move(out));
}

core::Result<std::vector<QuestDef>> LoadQuestDefsFromDir(std::string_view dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return FailVec(ErrorCode::NOT_FOUND,
                       std::string("quest config dir not found: ") + std::string(dir));
    }
    std::vector<std::string> files;
    for (auto it = std::filesystem::directory_iterator(dir, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const auto& entry = *it;
        if (entry.is_regular_file(ec) && entry.path().extension() == ".json") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<QuestDef> merged;
    for (const auto& f : files) {
        auto r = LoadQuestDefsFromFile(f);
        if (!r) return core::Result<std::vector<QuestDef>>::Fail(r.Err());
        for (auto& qd : r.Value()) merged.push_back(std::move(qd));
    }
    return core::Result<std::vector<QuestDef>>::Ok(std::move(merged));
}

}  // namespace mmo::game::quest
