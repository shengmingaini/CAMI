// client/resource/src/quality_preset.cpp —— 三档画质预设加载（配置化）。
//
// 自带极简 JSON 解析器（仅覆盖 quality.json 所需子集：对象 / 字符串 / 数字 /
// true / false / null / 标准空白与转义），不依赖 client_core 的 json 实现，
// 避免与本模块运行期解耦。解析失败即 Fail，绝不静默兜底。

#include "mmo/client/resource/quality_preset.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace mmo::client::resource {

namespace {

// ---- 极简 JSON 值 ----
struct JsonVal {
    enum class Type { Null, Bool, Num, Str, Arr, Obj } type = Type::Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<JsonVal> arr;
    std::vector<std::pair<std::string, JsonVal>> obj;

    const JsonVal* Find(std::string_view key) const {
        if (type != Type::Obj) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first.size() == key.size() &&
                std::equal(kv.first.begin(), kv.first.end(), key.begin())) {
                return &kv.second;
            }
        }
        return nullptr;
    }
    double AsNum(double fb = 0.0) const { return type == Type::Num ? num : fb; }
    std::int64_t AsInt(std::int64_t fb = 0) const {
        return type == Type::Num ? static_cast<std::int64_t>(num) : fb;
    }
};

// ---- 游标解析器 ----
class JsonParser {
public:
    explicit JsonParser(std::string_view text) : p_(text.data()), end_(text.data() + text.size()) {}

    core::Result<JsonVal> Parse() {
        SkipWs();
        JsonVal v;
        if (!ParseValue(v)) {
            return core::Result<JsonVal>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT, "quality.json: parse error", core::domain::kData));
        }
        SkipWs();
        if (p_ != end_) {
            return core::Result<JsonVal>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT, "quality.json: trailing garbage", core::domain::kData));
        }
        return core::Result<JsonVal>::Ok(std::move(v));
    }

private:
    const char* p_;
    const char* end_;

    void SkipWs() {
        while (p_ < end_) {
            const char c = *p_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p_;
            else break;
        }
    }

    bool ParseValue(JsonVal& out) {
        SkipWs();
        if (p_ >= end_) return false;
        const char c = *p_;
        if (c == '{') return ParseObject(out);
        if (c == '[') return ParseArray(out);
        if (c == '"') return ParseString(out);
        if (c == 't' || c == 'f') return ParseBool(out);
        if (c == 'n') return ParseNull(out);
        if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
        return false;
    }

    bool ParseObject(JsonVal& out) {
        out.type = JsonVal::Type::Obj;
        ++p_; // '{'
        SkipWs();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        while (true) {
            SkipWs();
            if (p_ >= end_ || *p_ != '"') return false;
            JsonVal key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_; // ':'
            JsonVal val;
            if (!ParseValue(val)) return false;
            out.obj.emplace_back(std::move(key.str), std::move(val));
            SkipWs();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }

    bool ParseArray(JsonVal& out) {
        out.type = JsonVal::Type::Arr;
        ++p_; // '['
        SkipWs();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        while (true) {
            JsonVal val;
            if (!ParseValue(val)) return false;
            out.arr.push_back(std::move(val));
            SkipWs();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }

    bool ParseString(JsonVal& out) {
        out.type = JsonVal::Type::Str;
        ++p_; // opening quote
        std::string s;
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"') { out.str = std::move(s); return true; }
            if (c == '\\') {
                if (p_ >= end_) return false;
                const char e = *p_++;
                switch (e) {
                    case '"': s.push_back('"'); break;
                    case '\\': s.push_back('\\'); break;
                    case '/': s.push_back('/'); break;
                    case 'b': s.push_back('\b'); break;
                    case 'f': s.push_back('\f'); break;
                    case 'n': s.push_back('\n'); break;
                    case 'r': s.push_back('\r'); break;
                    case 't': s.push_back('\t'); break;
                    case 'u': {
                        if (p_ + 4 > end_) return false;
                        std::uint32_t cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char h = *p_++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else return false;
                        }
                        // 仅处理基本 ASCII / Latin1，足够 quality.json
                        if (cp < 128) s.push_back(static_cast<char>(cp));
                        else s.push_back('?');
                        break;
                    }
                    default: return false;
                }
            } else {
                s.push_back(c);
            }
        }
        return false;
    }

    bool ParseNumber(JsonVal& out) {
        out.type = JsonVal::Type::Num;
        const char* start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        if (p_ < end_ && *p_ == '.') {
            ++p_;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-')) ++p_;
            while (p_ < end_ && *p_ >= '0' && *p_ <= '9') ++p_;
        }
        const std::string numStr(start, static_cast<std::size_t>(p_ - start));
        out.num = std::strtod(numStr.c_str(), nullptr);
        return true;
    }

    bool ParseBool(JsonVal& out) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "true", 4) == 0) {
            out.type = JsonVal::Type::Bool; out.b = true; p_ += 4; return true;
        }
        if (end_ - p_ >= 5 && std::strncmp(p_, "false", 5) == 0) {
            out.type = JsonVal::Type::Bool; out.b = false; p_ += 5; return true;
        }
        return false;
    }

    bool ParseNull(JsonVal& out) {
        if (end_ - p_ >= 4 && std::strncmp(p_, "null", 4) == 0) {
            out.type = JsonVal::Type::Null; p_ += 4; return true;
        }
        return false;
    }
};

std::array<QualityPreset, 3> g_presets = DefaultPresets();

void FillFromJson(QualityPreset& out, const JsonVal& obj) {
    if (obj.type != JsonVal::Type::Obj) return;
    auto getInt = [&](std::string_view k, std::uint32_t& dst) {
        const JsonVal* v = obj.Find(k); if (v) dst = static_cast<std::uint32_t>(v->AsInt(static_cast<std::int64_t>(dst)));
    };
    auto getFloat = [&](std::string_view k, float& dst) {
        const JsonVal* v = obj.Find(k); if (v) dst = static_cast<float>(v->AsNum(static_cast<double>(dst)));
    };
    auto getSize = [&](std::string_view k, std::size_t& dst) {
        const JsonVal* v = obj.Find(k); if (v) dst = static_cast<std::size_t>(v->AsInt(static_cast<std::int64_t>(dst)));
    };
    getInt("texture_max_size", out.texture_max_size);
    getInt("anisotropic", out.anisotropic);
    getFloat("lod_bias", out.lod_bias);
    getInt("shadow_quality", out.shadow_quality);
    getInt("max_visible_entities", out.max_visible_entities);
    getInt("max_particles", out.max_particles);
    getInt("net_update_hz", out.net_update_hz);
    getFloat("view_distance", out.view_distance);
    getInt("chunk_radius", out.chunk_radius);
    getSize("texture_budget_bytes", out.texture_budget_bytes);
    getSize("mesh_budget_bytes", out.mesh_budget_bytes);
}

}  // namespace

std::array<QualityPreset, 3> DefaultPresets() noexcept {
    std::array<QualityPreset, 3> out{};
    // Low
    out[0].level = QualityLevel::Low;
    out[0].texture_max_size = 512;
    out[0].anisotropic = 1;
    out[0].lod_bias = 1.0f;
    out[0].shadow_quality = 0;
    out[0].max_visible_entities = 50;
    out[0].max_particles = 200;
    out[0].net_update_hz = 10;
    out[0].view_distance = 80.0f;
    out[0].chunk_radius = 1;
    out[0].texture_budget_bytes = 256ULL * 1024 * 1024;
    out[0].mesh_budget_bytes = 128ULL * 1024 * 1024;
    // Medium
    out[1] = out[0];
    out[1].level = QualityLevel::Medium;
    out[1].texture_max_size = 1024;
    out[1].lod_bias = 0.0f;
    out[1].shadow_quality = 1;
    out[1].max_visible_entities = 150;
    out[1].max_particles = 1000;
    out[1].net_update_hz = 20;
    out[1].view_distance = 150.0f;
    out[1].chunk_radius = 2;
    out[1].texture_budget_bytes = 512ULL * 1024 * 1024;
    out[1].mesh_budget_bytes = 256ULL * 1024 * 1024;
    // High
    out[2] = out[1];
    out[2].level = QualityLevel::High;
    out[2].texture_max_size = 2048;
    out[2].shadow_quality = 2;
    out[2].max_visible_entities = 300;
    out[2].max_particles = 3000;
    out[2].net_update_hz = 30;
    out[2].view_distance = 250.0f;
    out[2].chunk_radius = 3;
    out[2].texture_budget_bytes = 1024ULL * 1024 * 1024;
    out[2].mesh_budget_bytes = 512ULL * 1024 * 1024;
    return out;
}

core::Result<void> LoadPresets(std::string_view json_path) {
    std::ifstream in(std::string(json_path.data(), json_path.size()), std::ios::binary);
    if (!in) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND,
            std::string("quality.json not found: ").append(json_path),
            core::domain::kData));
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();

    JsonParser parser(text);
    auto parsed = parser.Parse();
    if (!parsed) return core::Result<void>::Fail(std::move(parsed).Err());

    const JsonVal& root = parsed.Value();
    if (root.type != JsonVal::Type::Obj) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "quality.json: root not object", core::domain::kData));
    }
    static const char* kNames[3] = {"Low", "Medium", "High"};
    for (int i = 0; i < 3; ++i) {
        const JsonVal* sec = root.Find(kNames[i]);
        if (!sec) {
            return core::Result<void>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT,
                std::string("quality.json: missing section ").append(kNames[i]),
                core::domain::kData));
        }
        FillFromJson(g_presets[static_cast<std::size_t>(i)], *sec);
        g_presets[static_cast<std::size_t>(i)].level = static_cast<QualityLevel>(i);
    }
    return core::Result<void>::Ok();
}

core::Result<QualityPreset> GetPreset(QualityLevel level) {
    const std::size_t idx = static_cast<std::size_t>(level);
    if (idx >= g_presets.size()) {
        return core::Result<QualityPreset>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "invalid quality level", core::domain::kData));
    }
    return core::Result<QualityPreset>::Ok(g_presets[idx]);
}

std::string_view QualityLevelName(QualityLevel level) noexcept {
    switch (level) {
        case QualityLevel::Low:    return "Low";
        case QualityLevel::Medium: return "Medium";
        case QualityLevel::High:   return "High";
    }
    return "Unknown";
}

core::Result<QualityLevel> QualityLevelFromName(std::string_view name) {
    if (name == "Low")    return core::Result<QualityLevel>::Ok(QualityLevel::Low);
    if (name == "Medium") return core::Result<QualityLevel>::Ok(QualityLevel::Medium);
    if (name == "High")   return core::Result<QualityLevel>::Ok(QualityLevel::High);
    return core::Result<QualityLevel>::Fail(core::Error(
        core::ErrorCode::INVALID_ARGUMENT, "unknown quality name", core::domain::kData));
}

}  // namespace mmo::client::resource
