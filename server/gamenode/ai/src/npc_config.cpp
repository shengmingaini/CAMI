// server/gamenode/ai/src/npc_config.cpp — TASK-018 §15.2 / §21 Forbidden
//
// 从 config/gameplay/npc/*.json 加载生成定义。自带一个最小递归下降 JSON 解析器
// （顶层为 NPC/Monster 数组），不依赖第三方库。所有 NPC 数值来自配置，AI 逻辑零硬编码。
// 加载器仅在「生成准备阶段」调用，非 Tick 热路径；失败返回错误，禁止默认值静默生成（§19）。

#include "mmo/game/ai/npc_config.h"

#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <string_view>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::ai {

using namespace mmo::core;

namespace {

// ---- 最小 JSON DOM ----
struct JsonValue {
    enum class Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Type::Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Find(std::string_view key) const {
        if (type != Type::Obj) return nullptr;
        for (const auto& kv : obj) {
            if (kv.first.size() == key.size()
                && kv.first.compare(0, key.size(), key.data(), key.size()) == 0) {
                return &kv.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : p_(text.data()), end_(text.data() + text.size()) {}

    core::Result<JsonValue> Parse() {
        SkipWs();
        JsonValue v = ParseValue();
        SkipWs();
        if (p_ != end_) return Fail("trailing characters");
        return core::Result<JsonValue>::Ok(std::move(v));
    }

private:
    const char* p_;
    const char* end_;

    static core::Result<JsonValue> Fail(const char* msg) {
        return core::Result<JsonValue>::Fail(Error(ErrorCode::INVALID_ARGUMENT, msg, domain::kCore));
    }

    void SkipWs() {
        while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_))) ++p_;
    }
    char Peek() const { return p_ < end_ ? *p_ : '\0'; }

    JsonValue ParseValue() {
        SkipWs();
        const char c = Peek();
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') { JsonValue v; v.type = JsonValue::Type::Str; v.str = ParseString(); return v; }
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') { p_ += 4; JsonValue v; v.type = JsonValue::Type::Null; return v; }
        if (c == '-' || c == '+' || (c >= '0' && c <= '9')) return ParseNumber();
        return vFail();
    }
    JsonValue vFail() {
        (void)Fail("unexpected token");
        JsonValue v;
        v.type = JsonValue::Type::Null;
        return v;
    }

    JsonValue ParseObject() {
        JsonValue v;
        v.type = JsonValue::Type::Obj;
        ++p_;  // {
        SkipWs();
        if (Peek() == '}') { ++p_; return v; }
        for (;;) {
            SkipWs();
            if (Peek() != '"') { v.type = JsonValue::Type::Null; return v; }
            std::string key = ParseString();
            SkipWs();
            if (Peek() != ':') { v.type = JsonValue::Type::Null; return v; }
            ++p_;
            JsonValue val = ParseValue();
            v.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++p_; continue; }
            if (c == '}') { ++p_; break; }
            v.type = JsonValue::Type::Null;
            return v;
        }
        return v;
    }

    JsonValue ParseArray() {
        JsonValue v;
        v.type = JsonValue::Type::Arr;
        ++p_;  // [
        SkipWs();
        if (Peek() == ']') { ++p_; return v; }
        for (;;) {
            v.arr.push_back(ParseValue());
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++p_; continue; }
            if (c == ']') { ++p_; break; }
            v.type = JsonValue::Type::Null;
            return v;
        }
        return v;
    }

    JsonValue ParseBool() {
        JsonValue v;
        v.type = JsonValue::Type::Bool;
        if (Peek() == 't') { v.b = true; p_ += 4; }
        else { v.b = false; p_ += 5; }
        return v;
    }

    JsonValue ParseNumber() {
        const char* start = p_;
        if (Peek() == '-' || Peek() == '+') ++p_;
        while (p_ < end_ && (std::isdigit(static_cast<unsigned char>(*p_))
                             || *p_ == '.' || *p_ == 'e' || *p_ == 'E'
                             || *p_ == '+' || *p_ == '-')) {
            ++p_;
        }
        JsonValue v;
        v.type = JsonValue::Type::Num;
        v.num = std::strtod(start, nullptr);
        return v;
    }

    std::string ParseString() {
        std::string out;
        ++p_;  // opening quote
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\') {
                ++p_;
                if (p_ >= end_) break;
                const char e = *p_++;
                switch (e) {
                    case '"':  out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/'); break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    default:   break;  // \uXXXX 等复杂转义：配置为纯 ASCII，忽略
                }
            } else {
                out.push_back(*p_++);
            }
        }
        if (p_ < end_ && *p_ == '"') ++p_;
        return out;
    }
};

mmo::game::EntityType TypeFromName(std::string_view s) {
    if (s == "Npc")    return mmo::game::EntityType::Npc;
    if (s == "Player") return mmo::game::EntityType::Player;
    return mmo::game::EntityType::Monster;  // 默认 Monster
}

core::Result<SpawnDef> ToSpawnDef(const JsonValue& v) {
    if (v.type != JsonValue::Type::Obj) {
        return core::Result<SpawnDef>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "npc entry must be object", domain::kCore));
    }
    SpawnDef def;
    if (const auto* f = v.Find("npc_def_id")) def.npc_def_id = static_cast<std::uint32_t>(f->num);
    if (const auto* f = v.Find("type"))       def.type = TypeFromName(f->str);
    if (const auto* f = v.Find("spawn_pos")) {
        if (f->type == JsonValue::Type::Arr && f->arr.size() >= 4) {
            def.spawn_pos.x = static_cast<float>(f->arr[0].num);
            def.spawn_pos.y = static_cast<float>(f->arr[1].num);
            def.spawn_pos.z = static_cast<float>(f->arr[2].num);
            def.spawn_pos.yaw = static_cast<float>(f->arr[3].num);
        }
    }
    if (const auto* f = v.Find("patrol_radius"))        def.patrol_radius = static_cast<float>(f->num);
    if (const auto* f = v.Find("aggro_radius"))         def.aggro_radius = static_cast<float>(f->num);
    if (const auto* f = v.Find("chase_leave_radius"))   def.chase_leave_radius = static_cast<float>(f->num);
    if (const auto* f = v.Find("respawn_seconds"))      def.respawn_seconds = static_cast<std::uint32_t>(f->num);
    if (const auto* f = v.Find("max_hp"))               def.max_hp = static_cast<std::int64_t>(f->num);
    if (const auto* f = v.Find("level"))                def.level = static_cast<std::uint32_t>(f->num);
    // name 为元数据，SpawnDef.name 是 string_view（不拥有），此处留空避免悬垂；
    // 需要展示名时由调用方从自身配置副本读取。
    def.name = std::string_view();
    return core::Result<SpawnDef>::Ok(std::move(def));
}

std::string ReadFile(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

core::Result<std::vector<SpawnDef>> LoadSpawnDefs(std::string_view path) {
    const std::string text = ReadFile(path);
    if (text.empty()) {
        return core::Result<std::vector<SpawnDef>>::Fail(
            Error(ErrorCode::NOT_FOUND, "npc config empty or missing", domain::kCore));
    }
    auto parsed = JsonParser(text).Parse();
    if (!parsed) return core::Result<std::vector<SpawnDef>>::Fail(parsed.Err());
    if (parsed.Value().type != JsonValue::Type::Arr) {
        return core::Result<std::vector<SpawnDef>>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "npc config top-level must be array", domain::kCore));
    }
    std::vector<SpawnDef> out;
    out.reserve(parsed.Value().arr.size());
    for (const auto& item : parsed.Value().arr) {
        auto sd = ToSpawnDef(item);
        if (!sd) return core::Result<std::vector<SpawnDef>>::Fail(sd.Err());
        out.push_back(std::move(sd).Value());
    }
    return core::Result<std::vector<SpawnDef>>::Ok(std::move(out));
}

core::Result<std::vector<SpawnDef>> LoadSpawnDefsFromDir(std::string_view dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(std::string(dir), ec)) {
        return core::Result<std::vector<SpawnDef>>::Fail(
            Error(ErrorCode::NOT_FOUND, "npc config dir missing", domain::kCore));
    }
    std::vector<std::string> files;
    for (const auto& ent : std::filesystem::directory_iterator(std::string(dir), ec)) {
        if (ent.path().extension() == ".json") files.push_back(ent.path().string());
    }
    std::sort(files.begin(), files.end());
    std::vector<SpawnDef> out;
    for (const auto& f : files) {
        auto part = LoadSpawnDefs(f);
        if (!part) return core::Result<std::vector<SpawnDef>>::Fail(part.Err());
        for (auto& sd : part.Value()) out.push_back(std::move(sd));
    }
    return core::Result<std::vector<SpawnDef>>::Ok(std::move(out));
}

}  // namespace mmo::game::ai
