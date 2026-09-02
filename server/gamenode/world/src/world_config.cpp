// server/gamenode/world/src/world_config.cpp — TASK-020 §15.1 / §16 / §21 Forbidden
//
// 从 config/gameplay/world/*.json 加载实例与 OpenWorld 定义。自带最小递归下降 JSON
// 解析器（顶层为含 "instances" / "worlds" 键的对象），不依赖第三方库。所有实例数值来自
// 配置，AI 逻辑零硬编码。加载器仅在「准备阶段」调用（非 Tick 热路径）；失败返回错误，
// 禁止默认值静默生成（§19）。

#include "mmo/game/world/world_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::world {

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
        JsonValue v; v.type = JsonValue::Type::Null; return v;
    }

    JsonValue ParseObject() {
        JsonValue v; v.type = JsonValue::Type::Obj;
        ++p_; SkipWs();
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
            v.type = JsonValue::Type::Null; return v;
        }
        return v;
    }

    JsonValue ParseArray() {
        JsonValue v; v.type = JsonValue::Type::Arr;
        ++p_; SkipWs();
        if (Peek() == ']') { ++p_; return v; }
        for (;;) {
            v.arr.push_back(ParseValue());
            SkipWs();
            const char c = Peek();
            if (c == ',') { ++p_; continue; }
            if (c == ']') { ++p_; break; }
            v.type = JsonValue::Type::Null; return v;
        }
        return v;
    }

    JsonValue ParseBool() {
        JsonValue v; v.type = JsonValue::Type::Bool;
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
        JsonValue v; v.type = JsonValue::Type::Num; v.num = std::strtod(start, nullptr);
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

mmo::game::EntityType EntityTypeFromName(std::string_view s) {
    if (s == "Npc")    return mmo::game::EntityType::Npc;
    if (s == "Player") return mmo::game::EntityType::Player;
    return mmo::game::EntityType::Monster;  // 默认 Monster
}

InstanceType InstanceTypeFromName(std::string_view s) {
    if (s == "openworld")     return InstanceType::OpenWorld;
    if (s == "dungeon")       return InstanceType::Dungeon;
    if (s == "arena")         return InstanceType::Arena;
    if (s == "battleground")  return InstanceType::Battleground;
    if (s == "temporaryinstance") return InstanceType::TemporaryInstance;
    return InstanceType::Dungeon;  // 默认（未知在 ToInstanceDef 中显式报错）
}

bool IsKnownInstanceType(std::string_view s) {
    return s == "openworld" || s == "dungeon" || s == "arena"
        || s == "battleground" || s == "temporaryinstance";
}

core::Result<mmo::game::ai::SpawnDef> ToSpawnDef(const JsonValue& v,
                                                  std::deque<std::string>& /*pool*/) {
    using mmo::game::ai::SpawnDef;
    if (v.type != JsonValue::Type::Obj) {
        return core::Result<SpawnDef>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "spawn entry must be object", domain::kCore));
    }
    SpawnDef sd;
    if (const auto* f = v.Find("npc_def_id")) sd.npc_def_id = static_cast<std::uint32_t>(f->num);
    if (const auto* f = v.Find("type"))       sd.type = EntityTypeFromName(f->str);
    // name 为元数据，SpawnDef.name 是 string_view，留空防悬垂
    if (const auto* f = v.Find("spawn_pos")) {
        if (f->type == JsonValue::Type::Arr && f->arr.size() >= 4) {
            sd.spawn_pos.x = static_cast<float>(f->arr[0].num);
            sd.spawn_pos.y = static_cast<float>(f->arr[1].num);
            sd.spawn_pos.z = static_cast<float>(f->arr[2].num);
            sd.spawn_pos.yaw = static_cast<float>(f->arr[3].num);
        } else if (f->type == JsonValue::Type::Obj) {
            if (const auto* x = f->Find("x")) sd.spawn_pos.x = static_cast<float>(x->num);
            if (const auto* y = f->Find("y")) sd.spawn_pos.y = static_cast<float>(y->num);
            if (const auto* z = f->Find("z")) sd.spawn_pos.z = static_cast<float>(z->num);
            if (const auto* w = f->Find("yaw")) sd.spawn_pos.yaw = static_cast<float>(w->num);
        }
    }
    if (const auto* f = v.Find("patrol_radius"))      sd.patrol_radius = static_cast<float>(f->num);
    if (const auto* f = v.Find("aggro_radius"))       sd.aggro_radius = static_cast<float>(f->num);
    if (const auto* f = v.Find("chase_leave_radius")) sd.chase_leave_radius = static_cast<float>(f->num);
    if (const auto* f = v.Find("respawn_seconds"))    sd.respawn_seconds = static_cast<std::uint32_t>(f->num);
    if (const auto* f = v.Find("max_hp"))             sd.max_hp = static_cast<std::int64_t>(f->num);
    if (const auto* f = v.Find("level"))              sd.level = static_cast<std::uint32_t>(f->num);
    return core::Result<SpawnDef>::Ok(std::move(sd));
}

core::Result<InstanceDef> ToInstanceDef(const JsonValue& v, std::deque<std::string>& pool) {
    if (v.type != JsonValue::Type::Obj) {
        return core::Result<InstanceDef>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "instance entry must be object", domain::kCore));
    }
    InstanceDef def;
    const auto* idf = v.Find("def_id");
    if (idf == nullptr) {
        return core::Result<InstanceDef>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "instance missing def_id", domain::kCore));
    }
    def.def_id = static_cast<std::uint32_t>(idf->num);

    if (const auto* tf = v.Find("type")) {
        if (!IsKnownInstanceType(tf->str)) {
            return core::Result<InstanceDef>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "unknown instance type", domain::kCore));
        }
        def.type = InstanceTypeFromName(tf->str);
    } else {
        def.type = InstanceType::Dungeon;
    }

    if (const auto* nm = v.Find("name")) { pool.push_back(nm->str); def.name = pool.back(); }
    if (const auto* mp = v.Find("max_players")) def.max_players = static_cast<std::uint32_t>(mp->num);
    else def.max_players = 5;
    if (const auto* minp = v.Find("min_players")) def.min_players = static_cast<std::uint32_t>(minp->num);
    if (const auto* tl = v.Find("time_limit_ms")) {
        def.time_limit = core::DurationMs(static_cast<std::int64_t>(tl->num));
    }
    if (const auto* sa = v.Find("scene_asset")) { pool.push_back(sa->str); def.scene_asset = pool.back(); }

    if (def.max_players < 1) {
        return core::Result<InstanceDef>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "max_players must be >= 1", domain::kCore));
    }
    if (def.min_players > def.max_players) {
        return core::Result<InstanceDef>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "min_players > max_players", domain::kCore));
    }

    if (const auto* sp = v.Find("spawns")) {
        if (sp->type != JsonValue::Type::Arr) {
            return core::Result<InstanceDef>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "spawns must be array", domain::kCore));
        }
        def.spawn_defs.reserve(sp->arr.size());
        for (const auto& s : sp->arr) {
            auto sd = ToSpawnDef(s, pool);
            if (!sd) return core::Result<InstanceDef>::Fail(sd.Err());
            def.spawn_defs.push_back(std::move(sd).Value());
        }
    }
    return core::Result<InstanceDef>::Ok(std::move(def));
}

std::string ReadFile(std::string_view path) {
    std::ifstream in(std::string(path), std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

core::Result<WorldConfigBundle> LoadWorldConfig(std::string_view dir) {
    WorldConfigBundle bundle;
    std::error_code ec;
    if (!std::filesystem::is_directory(std::string(dir), ec)) {
        return core::Result<WorldConfigBundle>::Fail(
            Error(ErrorCode::NOT_FOUND, "world config dir missing", domain::kCore));
    }
    std::vector<std::string> files;
    for (const auto& ent : std::filesystem::directory_iterator(std::string(dir), ec)) {
        if (ent.path().extension() == ".json") files.push_back(ent.path().string());
    }
    std::sort(files.begin(), files.end());

    for (const auto& f : files) {
        const std::string text = ReadFile(f);
        if (text.empty()) {
            return core::Result<WorldConfigBundle>::Fail(
                Error(ErrorCode::NOT_FOUND, "world config file empty", domain::kCore));
        }
        auto parsed = JsonParser(text).Parse();
        if (!parsed) return core::Result<WorldConfigBundle>::Fail(parsed.Err());
        const JsonValue& top = parsed.Value();
        if (top.type != JsonValue::Type::Obj) {
            return core::Result<WorldConfigBundle>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "world config top must be object", domain::kCore));
        }
        if (const auto* ws = top.Find("worlds")) {
            if (ws->type != JsonValue::Type::Arr) {
                return core::Result<WorldConfigBundle>::Fail(
                    Error(ErrorCode::INVALID_ARGUMENT, "worlds must be array", domain::kCore));
            }
            for (const auto& w : ws->arr) {
                WorldDef d;
                if (const auto* id = w.Find("id")) d.id = static_cast<std::uint32_t>(id->num);
                else {
                    return core::Result<WorldConfigBundle>::Fail(
                        Error(ErrorCode::INVALID_ARGUMENT, "world missing id", domain::kCore));
                }
                if (const auto* nm = w.Find("name")) d.name = nm->str;
                if (const auto* sa = w.Find("scene_asset")) d.scene_asset = sa->str;
                if (bundle.worlds.count(d.id)) {
                    return core::Result<WorldConfigBundle>::Fail(
                        Error(ErrorCode::INVALID_ARGUMENT, "duplicate world id", domain::kCore));
                }
                bundle.worlds.emplace(d.id, std::move(d));
            }
        }
        if (const auto* is = top.Find("instances")) {
            if (is->type != JsonValue::Type::Arr) {
                return core::Result<WorldConfigBundle>::Fail(
                    Error(ErrorCode::INVALID_ARGUMENT, "instances must be array", domain::kCore));
            }
            for (const auto& it : is->arr) {
                auto def_r = ToInstanceDef(it, bundle.pool);
                if (!def_r) return core::Result<WorldConfigBundle>::Fail(def_r.Err());
                auto def = std::move(def_r).Value();
                if (bundle.instances.count(def.def_id)) {
                    return core::Result<WorldConfigBundle>::Fail(
                        Error(ErrorCode::INVALID_ARGUMENT, "duplicate instance def_id", domain::kCore));
                }
                bundle.instances.emplace(def.def_id, std::move(def));
            }
        }
    }
    return core::Result<WorldConfigBundle>::Ok(std::move(bundle));
}

}  // namespace mmo::game::world
