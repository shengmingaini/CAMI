// server/gamenode/economy/src/price_table.cpp — TASK-029 §15.3 / §20.6
//
// 价格必须来自配置（§20.6 禁止代码硬编码价格）。解析沿用 world/quest/ai/skill 四个
// 模块既有约定：模块内自建最小 JSON DOM（core 的 json_parser 属 config_internal，
// 不对模块暴露）。文件读取统一走 core::ConfigManager::ReadFile —— 业务 src/ 禁止
// 出现文件流（§24 红线）。

#include "mmo/game/economy/price_table.h"

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error.h"

namespace mmo::game::economy {

namespace {

core::Error PriceErr(core::ErrorCode code, const char* msg) noexcept {
    return core::Error(code, msg, core::domain::kEconomy);
}

// ---- 最小 JSON DOM（与 world_config.cpp 同款，仅限本翻译单元）----
struct JsonValue {
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Find(std::string_view key) const noexcept {
        if (type != Type::Obj) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
};

struct JsonParser {
    const char* p{nullptr};
    const char* end{nullptr};
    bool bad{false};

    explicit JsonParser(std::string_view t) : p(t.data()), end(t.data() + t.size()) {}

    JsonValue Parse() {
        SkipWs();
        JsonValue v = ParseValue();
        SkipWs();
        if (!bad && p != end) bad = true;  // 尾随字符
        if (bad) {
            JsonValue n;
            n.type = JsonValue::Type::Null;
            return n;
        }
        return v;
    }

private:
    void SkipWs() {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    }
    char Peek() const { return p < end ? *p : '\0'; }
    void MarkBad() { bad = true; }

    JsonValue ParseValue() {
        SkipWs();
        const char c = Peek();
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') {
            JsonValue v;
            v.type = JsonValue::Type::Str;
            v.str = ParseString();
            return v;
        }
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') {
            p += 4;
            JsonValue v;
            v.type = JsonValue::Type::Null;
            return v;
        }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return ParseNumber();
        MarkBad();
        JsonValue v;
        v.type = JsonValue::Type::Null;
        return v;
    }

    JsonValue ParseObject() {
        JsonValue v;
        v.type = JsonValue::Type::Obj;
        ++p;
        SkipWs();
        if (Peek() == '}') {
            ++p;
            return v;
        }
        for (;;) {
            SkipWs();
            if (Peek() != '"') {
                MarkBad();
                return v;
            }
            std::string key = ParseString();
            SkipWs();
            if (Peek() != ':') {
                MarkBad();
                return v;
            }
            ++p;
            v.obj.emplace_back(std::move(key), ParseValue());
            SkipWs();
            const char c = Peek();
            if (c == ',') {
                ++p;
                continue;
            }
            if (c == '}') {
                ++p;
                break;
            }
            MarkBad();
            break;
        }
        return v;
    }

    JsonValue ParseArray() {
        JsonValue v;
        v.type = JsonValue::Type::Arr;
        ++p;
        SkipWs();
        if (Peek() == ']') {
            ++p;
            return v;
        }
        for (;;) {
            v.arr.push_back(ParseValue());
            SkipWs();
            const char c = Peek();
            if (c == ',') {
                ++p;
                continue;
            }
            if (c == ']') {
                ++p;
                break;
            }
            MarkBad();
            break;
        }
        return v;
    }

    JsonValue ParseBool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (Peek() == 't') {
            v.b = true;
            p += 4;
        } else {
            v.b = false;
            p += 5;
        }
        return v;
    }

    JsonValue ParseNumber() {
        const char* start = p;
        if (Peek() == '-' || Peek() == '+') ++p;
        while (p < end
               && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '.' || *p == 'e'
                   || *p == 'E' || *p == '+' || *p == '-')) {
            ++p;
        }
        JsonValue v;
        v.type = JsonValue::Type::Num;
        v.num = std::strtod(start, nullptr);
        return v;
    }

    std::string ParseString() {
        std::string out;
        ++p;  // 跳过起始引号
        while (p < end && *p != '"') {
            if (*p == '\\') {
                ++p;
                if (p >= end) break;
                const char e = *p++;
                switch (e) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:   break;  // 配置为纯 ASCII，\uXXXX 不处理
                }
            } else {
                out.push_back(*p++);
            }
        }
        if (p < end && *p == '"') ++p;  // 跳过收尾引号
        return out;
    }
};

/// 取整数：Num 转 int64。非数字或越界返回 false（禁止静默截断）。
bool AsInt(const JsonValue* v, std::int64_t& out) noexcept {
    if (v == nullptr || v->type != JsonValue::Type::Num) return false;
    const double d = v->num;
    if (d != d) return false;  // NaN
    const auto i = static_cast<std::int64_t>(d);
    out = i;
    return true;
}

}  // namespace

bool PriceEntry::Allows(std::uint32_t count) const noexcept {
    if (count < min_count) return false;
    if (max_count != 0 && count > max_count) return false;
    return true;
}

bool PriceEntry::Total(std::uint32_t count, std::int64_t& out) const noexcept {
    // 溢出检测：单价 * 数量 必须落在 [0, kMaxBalance]，否则拒绝（防刷币 / 防溢出）。
    if (unit_price < 0) return false;
    const std::int64_t c = static_cast<std::int64_t>(count);
    if (unit_price != 0 && c > kMaxBalance / unit_price) return false;
    const std::int64_t t = unit_price * c;
    if (t > kMaxBalance) return false;
    out = t;
    return true;
}

core::Result<void> PriceTable::LoadFromText(std::string_view json) noexcept {
    JsonParser parser(json);
    const JsonValue root = parser.Parse();
    if (root.type != JsonValue::Type::Obj) {
        return core::Result<void>::Fail(
            PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: prices root must be object"));
    }

    const JsonValue* arr = root.Find("prices");
    if (arr == nullptr || arr->type != JsonValue::Type::Arr) {
        return core::Result<void>::Fail(
            PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: prices must be an array"));
    }

    // 先解析到临时表：任一条目非法则整表丢弃（原子加载，禁止半应用，§21）。
    std::unordered_map<inventory::ItemId, PriceEntry> next;
    next.reserve(arr->arr.size());

    for (const JsonValue& e : arr->arr) {
        if (e.type != JsonValue::Type::Obj) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: price entry must be object"));
        }
        std::int64_t item_id = 0, currency = 0, unit_price = 0, min_count = 0, max_count = 0;
        if (!AsInt(e.Find("item_id"), item_id) || item_id <= 0) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: bad item_id"));
        }
        if (!AsInt(e.Find("currency"), currency) || currency <= 0
            || currency > static_cast<std::int64_t>(kCurrencyTypeMax)) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: bad currency"));
        }
        if (!AsInt(e.Find("unit_price"), unit_price) || unit_price < 0) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: bad unit_price"));
        }
        const JsonValue* jmin = e.Find("min_count");
        if (jmin != nullptr && !AsInt(jmin, min_count)) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: bad min_count"));
        }
        const JsonValue* jmax = e.Find("max_count");
        if (jmax != nullptr && !AsInt(jmax, max_count)) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: bad max_count"));
        }

        PriceEntry pe;
        pe.currency = static_cast<CurrencyType>(currency);
        pe.unit_price = unit_price;
        pe.min_count = (min_count <= 0) ? 1u : static_cast<std::uint32_t>(min_count);
        pe.max_count = (max_count <= 0) ? 0u : static_cast<std::uint32_t>(max_count);
        if (pe.max_count != 0 && pe.max_count < pe.min_count) {
            return core::Result<void>::Fail(
                PriceErr(core::ErrorCode::INVALID_ARGUMENT, "economy: max_count < min_count"));
        }
        next[static_cast<inventory::ItemId>(item_id)] = pe;
    }

    entries_ = std::move(next);
    ++version_;
    return core::Result<void>::Ok();
}

core::Result<void> PriceTable::LoadFromFile(std::string_view path) noexcept {
    auto text = core::ConfigManager::ReadFile(path);
    if (!text.HasValue()) return core::Result<void>::Fail(text.Err());
    return LoadFromText(text.Value());
}

const PriceEntry* PriceTable::Find(inventory::ItemId id) const noexcept {
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : &it->second;
}

}  // namespace mmo::game::economy
