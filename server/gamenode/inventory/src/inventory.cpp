// server/gamenode/inventory/src/inventory.cpp — TASK-017 §7 / §15 背包容器与物品配置表
//
// Inventory：定长槽位数组 + 空闲槽栈，禁止无界增长（§21）。
// ItemDefStore：从 config/gameplay/items/*.json 加载静态物品定义。
//   不依赖 core::ConfigManager 全局快照（多文件同键合并会冲突），故用受限 JSON 解析：
//   支持「单对象」与「顶层数组」，扁平读取已知 key（def_id / name / max_stack / ... /
//   attr_bonus 数组）。物品是我方编写的受控配置，解析器只需覆盖此子集。

#include "mmo/game/inventory/inventory.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::inventory {

namespace {

core::Error NotFound(const char* what) {
    return core::Error(core::ErrorCode::NOT_FOUND, what, core::domain::kCore);
}
core::Error BadArg(const char* what) {
    return core::Error(core::ErrorCode::INVALID_ARGUMENT, what, core::domain::kCore);
}
core::Error Busy(const char* what) {
    return core::Error(core::ErrorCode::BUSY, what, core::domain::kCore);
}

// ---- 极简 JSON 扫描（仅覆盖受控物品配置子集） ----

const char* SkipWs(const char* p, const char* e) noexcept {
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

// 从 '"' 读取字符串（支持 \" \\ \/ 与 \uXXXX）到 out，p 越过闭合引号。
bool ReadString(const char*& p, const char* e, std::string& out) {
    if (p >= e || *p != '"') return false;
    ++p;
    out.clear();
    while (p < e) {
        const char c = *p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (p >= e) return false;
            const char d = *p++;
            switch (d) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (p + 4 > e) return false;
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = *p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    // 仅支持 BMP 基本平面（足够物品名）；代理对不在此任务范围。
                    out += static_cast<char>(static_cast<unsigned char>((cp >> 8) & 0xFF));
                    out += static_cast<char>(static_cast<unsigned char>(cp & 0xFF));
                    break;
                }
                default: out += d; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

// 从数字起点读取 int64（支持负号），p 越过数字。
bool ReadInt(const char*& p, const char* e, std::int64_t& out) {
    const char* s = p;
    bool neg = false;
    if (p < e && *p == '-') { neg = true; ++p; }
    if (p >= e || !(*p >= '0' && *p <= '9')) { p = s; return false; }
    std::int64_t v = 0;
    while (p < e && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
    out = neg ? -v : v;
    return true;
}

// 在 text 中查找 "key"（带引号）并返回冒号后的位置（失败返回 nullptr）。
const char* FindKey(std::string_view text, std::string_view key) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    auto pos = text.find(needle);
    if (pos == std::string_view::npos) return nullptr;
    return text.data() + pos + needle.size();
}

bool ReadFieldInt(std::string_view text, std::string_view key, std::int64_t& out) {
    const char* kp = FindKey(text, key);
    if (kp == nullptr) return false;
    const char* e = text.data() + text.size();
    kp = SkipWs(kp, e);
    if (kp >= e || *kp != ':') return false;
    kp = SkipWs(kp + 1, e);
    return ReadInt(kp, e, out);
}

bool ReadFieldString(std::string_view text, std::string_view key, std::string& out) {
    const char* kp = FindKey(text, key);
    if (kp == nullptr) return false;
    const char* e = text.data() + text.size();
    kp = SkipWs(kp, e);
    if (kp >= e || *kp != ':') return false;
    kp = SkipWs(kp + 1, e);
    return ReadString(kp, e, out);
}

// 读取 attr_bonus 数组（最多 kAttrCount 个 int64）。
bool ReadFieldIntArray(std::string_view text, std::string_view key,
                       std::array<std::int64_t, role::kAttrCount>& out) {
    const char* kp = FindKey(text, key);
    if (kp == nullptr) return false;
    const char* e = text.data() + text.size();
    kp = SkipWs(kp, e);
    if (kp >= e || *kp != ':') return false;
    kp = SkipWs(kp + 1, e);
    if (kp >= e || *kp != '[') return false;
    kp = SkipWs(kp + 1, e);
    std::size_t idx = 0;
    out.fill(0);
    while (kp < e && *kp != ']') {
        std::int64_t x = 0;
        if (!ReadInt(kp, e, x)) return false;
        if (idx < role::kAttrCount) out[idx++] = x;
        kp = SkipWs(kp, e);
        if (*kp == ',') { ++kp; kp = SkipWs(kp, e); }
        else if (*kp == ']') { ++kp; break; }
        else return false;
    }
    return true;
}

// 解析一个对象子串（text 以 '{' 开始、到匹配 '}' 结束）为 StoredDef。
core::Result<void> ParseItemDef(std::string_view text, ItemDefStore::StoredDef& out) {
    std::int64_t def_id = 0, max_stack = 1, item_type = 0, required_level = 0;
    std::int64_t max_durability = 0, equip_slots = 0;
    if (!ReadFieldInt(text, "def_id", def_id)) return core::Result<void>::Fail(BadArg("missing def_id"));
    if (def_id <= 0) return core::Result<void>::Fail(BadArg("def_id must be > 0"));

    std::string name;
    if (!ReadFieldString(text, "name", name)) name = "item";
    ReadFieldInt(text, "max_stack", max_stack);
    ReadFieldInt(text, "item_type", item_type);
    ReadFieldInt(text, "required_level", required_level);
    ReadFieldInt(text, "max_durability", max_durability);
    ReadFieldInt(text, "equip_slots", equip_slots);

    std::array<std::int64_t, role::kAttrCount> bonus{};
    ReadFieldIntArray(text, "attr_bonus", bonus);

    out.name = std::move(name);
    ItemDef& d = out.def;
    d.def_id = static_cast<ItemId>(def_id);
    d.name = out.name;  // string_view 指向 StoredDef::name（生命周期由本表持有）
    d.max_stack = max_stack > 0 ? static_cast<std::uint32_t>(max_stack) : 1u;
    d.item_type = static_cast<std::uint8_t>(item_type);
    d.required_level = static_cast<std::uint32_t>(required_level < 0 ? 0 : required_level);
    d.max_durability = static_cast<std::uint32_t>(max_durability < 0 ? 0 : max_durability);
    d.equip_slots = static_cast<std::uint16_t>(equip_slots);
    d.attr_bonus = bonus;
    return core::Result<void>::Ok();
}

// 解析整个文件文本：支持顶层对象或顶层数组。
core::Result<void> ParseFile(std::string_view text, std::vector<ItemDefStore::StoredDef>& out_items) {
    const char* p = text.data();
    const char* e = p + text.size();
    p = SkipWs(p, e);
    if (p >= e) return core::Result<void>::Fail(BadArg("empty file"));

    if (*p == '[') {
        ++p;
        p = SkipWs(p, e);
        while (p < e && *p != ']') {
            if (*p != '{') return core::Result<void>::Fail(BadArg("array element not object"));
            int depth = 0;
            const char* objstart = p;
            const char* q = p;
            for (; q < e; ++q) {
                if (*q == '{') ++depth;
                else if (*q == '}') {
                    --depth;
                    if (depth == 0) { ++q; break; }
                }
            }
            if (depth != 0) return core::Result<void>::Fail(BadArg("unbalanced braces"));
            ItemDefStore::StoredDef sd;
            auto r = ParseItemDef(std::string_view(objstart, static_cast<std::size_t>(q - objstart)), sd);
            if (!r.HasValue()) return r;
            out_items.push_back(std::move(sd));
            p = SkipWs(q, e);
            if (*p == ',') { ++p; p = SkipWs(p, e); }
            else if (*p == ']') break;
            else return core::Result<void>::Fail(BadArg("expected ',' or ']' in array"));
        }
        return core::Result<void>::Ok();
    }
    if (*p == '{') {
        int depth = 0;
        const char* objstart = p;
        const char* q = p;
        for (; q < e; ++q) {
            if (*q == '{') ++depth;
            else if (*q == '}') {
                --depth;
                if (depth == 0) { ++q; break; }
            }
        }
        if (depth != 0) return core::Result<void>::Fail(BadArg("unbalanced braces"));
        ItemDefStore::StoredDef sd;
        auto r = ParseItemDef(std::string_view(objstart, static_cast<std::size_t>(q - objstart)), sd);
        if (!r.HasValue()) return r;
        out_items.push_back(std::move(sd));
        return core::Result<void>::Ok();
    }
    return core::Result<void>::Fail(BadArg("not a JSON object or array"));
}

}  // namespace

// ---------------------------------------------------------------------------
// Inventory
// ---------------------------------------------------------------------------

core::Result<Inventory::AddOutcome> Inventory::Add(const ItemDef& def, std::uint32_t count) noexcept {
    AddOutcome out;
    if (count == 0) return core::Result<AddOutcome>::Fail(BadArg("count==0"));

    std::uint32_t remaining = count;
    // 1) 堆叠到已有同 def_id 且未满的栈
    if (def.max_stack > 1) {
        for (SlotIndex i = 0; i < kMaxInventorySlots; ++i) {
            ItemStack& s = bag_[i];
            if (s.guid != 0 && s.def_id == def.def_id && s.count < def.max_stack) {
                const std::uint32_t room = def.max_stack - s.count;
                const std::uint32_t add = (room < remaining) ? room : remaining;
                s.count += add;
                remaining -= add;
                if (out.guid == 0) out.guid = s.guid;
                if (remaining == 0) break;
            }
        }
    }
    // 2) 剩余开新栈（每栈 ≤ max_stack）
    while (remaining > 0) {
        const SlotIndex idx = AllocateSlot();
        if (idx == kMaxInventorySlots) break;  // 无空槽
        const std::uint32_t add = (def.max_stack < remaining) ? def.max_stack : remaining;
        const ItemGuid g = NewItemGuid();  // 每新栈全局唯一 guid
        bag_[idx] = ItemStack{def.def_id, add, def.max_durability, g};
        if (out.guid == 0) out.guid = g;
        remaining -= add;
        ++used_;
    }

    out.added = count - remaining;
    if (out.added == 0) {
        // 背包真满且无法堆叠任何 → 拒绝（物品不产生，§19 防凭空产生/丢失）
        return core::Result<AddOutcome>::Fail(Busy("inventory full"));
    }
    return core::Result<AddOutcome>::Ok(out);
}

core::Result<std::uint32_t> Inventory::AddExisting(const ItemDef& def, const ItemStack& st) noexcept {
    if (st.guid == 0) return core::Result<std::uint32_t>::Fail(BadArg("null guid"));
    if (Find(st.guid) != kMaxInventorySlots) return core::Result<std::uint32_t>::Ok(st.count);
    const SlotIndex idx = AllocateSlot();
    if (idx == kMaxInventorySlots) {
        // 背包满 → 拒绝，物品不丢失（调用方据此中止 Unequip，物品仍留在装备槽）
        return core::Result<std::uint32_t>::Fail(Busy("inventory full"));
    }
    bag_[idx] = st;  // 复制，保留 guid + durability（装备 max_stack=1，独占空槽）
    ++used_;
    (void)def;
    return core::Result<std::uint32_t>::Ok(st.count);
}

core::Result<std::uint32_t> Inventory::Remove(ItemGuid guid, std::uint32_t count) noexcept {
    if (count == 0) return core::Result<std::uint32_t>::Fail(BadArg("count==0"));
    const SlotIndex i = Find(guid);
    if (i == kMaxInventorySlots) return core::Result<std::uint32_t>::Fail(NotFound("guid not in bag"));
    ItemStack& s = bag_[i];
    if (count > s.count) {
        // 拒绝扣减，防负数（§19 不扣减）
        return core::Result<std::uint32_t>::Fail(BadArg("count>held"));
    }
    s.count -= count;
    if (s.count == 0) {
        s = ItemStack{};        // 清空槽（guid=0）
        free_.push_back(i);      // 回收
        --used_;
    }
    return core::Result<std::uint32_t>::Ok(count);
}

SlotIndex Inventory::Find(ItemGuid guid) const noexcept {
    if (guid == 0) return kMaxInventorySlots;
    for (SlotIndex i = 0; i < kMaxInventorySlots; ++i) {
        if (bag_[i].guid == guid) return i;
    }
    return kMaxInventorySlots;
}

std::uint64_t Inventory::TotalCount() const noexcept {
    std::uint64_t t = 0;
    for (SlotIndex i = 0; i < kMaxInventorySlots; ++i) {
        if (bag_[i].guid != 0) t += bag_[i].count;
    }
    return t;
}

bool Inventory::IsEquipped(ItemGuid guid) const noexcept {
    if (guid == 0) return false;
    for (std::size_t s = 0; s < kEquipSlotCount; ++s) {
        if (equipped_[s].guid == guid) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ItemDefStore
// ---------------------------------------------------------------------------

core::Result<void> ItemDefStore::LoadDir(const char* dir) {
    namespace fs = std::filesystem;
    const fs::path d = dir;
    if (!fs::is_directory(d)) return core::Result<void>::Fail(NotFound("items dir not found"));

    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& ent : fs::directory_iterator(d, ec)) {
        if (ent.is_regular_file() && ent.path().extension() == ".json") {
            files.push_back(ent.path());
        }
    }
    if (ec) return core::Result<void>::Fail(BadArg("directory iterate error"));

    for (const auto& fp : files) {
        std::ifstream in(fp, std::ios::binary);
        if (!in) return core::Result<void>::Fail(NotFound("cannot open item file"));
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();

        std::vector<StoredDef> items;
        auto r = ParseFile(text, items);
        if (!r.HasValue()) return r;

        for (auto& sd : items) {
            if (defs_.find(sd.def.def_id) != defs_.end()) {
                return core::Result<void>::Fail(BadArg("duplicate def_id"));
            }
            defs_.emplace(sd.def.def_id, std::move(sd));
        }
    }
    return core::Result<void>::Ok();
}

const ItemDef* ItemDefStore::Lookup(ItemId def_id) const noexcept {
    auto it = defs_.find(def_id);
    if (it == defs_.end()) return nullptr;
    return &it->second.def;
}

}  // namespace mmo::game::inventory
