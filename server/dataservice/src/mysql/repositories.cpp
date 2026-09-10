// server/dataservice/src/mysql/repositories.cpp
//
// TASK-028 §8 / §15.8 · 7 张逻辑表的 TableSpec 与 EntityCodec 特化。

#include "mmo/data/mysql/repositories.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mmo/data/mysql/password_hash.h"
#include "mmo/data/mysql/sql_builder.h"

namespace mmo::data::mysql {

namespace {

core::Error Err(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, core::domain::kData);
}

std::uint64_t AsU64(const std::optional<std::int64_t>& v) {
    return (v.has_value() && *v > 0) ? static_cast<std::uint64_t>(*v) : 0;
}

std::uint32_t AsU32(const std::optional<std::int64_t>& v) {
    return (v.has_value() && *v > 0) ? static_cast<std::uint32_t>(*v) : 0;
}

std::string AsStr(const std::optional<std::string>& v) { return v.value_or(std::string{}); }

/// 单行表：把一组参数包成「一行」结果。
core::Result<std::vector<MySqlParams>> OneRow(core::Result<MySqlParams>&& p) {
    if (!p.HasValue()) return core::Result<std::vector<MySqlParams>>::Fail(p.Err());
    std::vector<MySqlParams> out;
    out.push_back(std::move(p.Value()));
    return core::Result<std::vector<MySqlParams>>::Ok(std::move(out));
}

/// 多行表：每行一组参数（含 key 列与 sub_key 列）。
core::Result<std::vector<MySqlParams>> MultiRows(std::vector<MySqlParams>&& rows) {
    return core::Result<std::vector<MySqlParams>>::Ok(std::move(rows));
}

core::Result<RowView> FirstView(const std::vector<MySqlRow>& rows, const TableSpec& spec,
                                const char* what) {
    if (rows.empty()) {
        return core::Result<RowView>::Fail(Err(core::ErrorCode::NOT_FOUND, what));
    }
    RowView v = MakeRowView(spec, rows.front());
    if (v.empty()) {
        return core::Result<RowView>::Fail(
            Err(core::ErrorCode::INTERNAL_ERROR, "row shape mismatch (column count)"));
    }
    return core::Result<RowView>::Ok(std::move(v));
}

}  // namespace

// ============================ TableSpec ============================

TableSpec AccountTable() {
    return TableSpec{tables::kAccount, "account_id", "",
                     {"account_id", "username", "password_hash", "created_at", "version"},
                     "version"};
}

TableSpec CharacterTable() {
    return TableSpec{tables::kCharacter, "char_id", "",
                     {"char_id", "account_id", "name", "level", "exp", "attrs_json", "version"},
                     "version"};
}

TableSpec InventoryTable() {
    return TableSpec{tables::kInventory, "char_id", "slot",
                     {"char_id", "slot", "item_guid", "item_def_id", "count", "durability",
                      "version"},
                     "version"};
}

TableSpec EquipmentTable() {
    return TableSpec{tables::kEquipment, "char_id", "slot",
                     {"char_id", "slot", "item_guid", "version"}, "version"};
}

TableSpec QuestTable() {
    return TableSpec{tables::kQuest, "char_id", "quest_id",
                     {"char_id", "quest_id", "status", "progress_json", "version"}, "version"};
}

TableSpec GuildTable() {
    return TableSpec{tables::kGuild, "guild_id", "",
                     {"guild_id", "name", "leader_id", "member_count", "version"}, "version"};
}

TableSpec MailTable() {
    return TableSpec{tables::kMail, "mail_id", "",
                     {"mail_id", "receiver_id", "sender_id", "payload", "status", "expire_at",
                      "version"},
                     "version"};
}

// ============================ Account ============================

core::Result<std::uint64_t> EntityCodec<Account>::KeyOf(const Account& e) {
    if (e.account_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "account_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.account_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Account>::ToRows(const Account& e,
                                                                   const TableSpec& s) {
    std::unordered_map<std::string, MySqlValue> m;
    m["account_id"] = MySqlValue::Uint(e.account_id);
    m["username"] = MySqlValue::Text(e.username);
    // 只允许写入 argon2id 编码串（§20.7）：明文入库直接被拒。
    if (!IsArgon2idEncoded(e.password_hash)) {
        return core::Result<std::vector<MySqlParams>>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "account password must be salted hash"));
    }
    m["password_hash"] = MySqlValue::Text(e.password_hash);
    m["created_at"] = MySqlValue::Text(e.created_at);
    m["version"] = MySqlValue::Uint(e.version);
    return OneRow(OrderParams(s, m));
}

core::Result<Account> EntityCodec<Account>::FromRows(const std::vector<MySqlRow>& rows,
                                                     const TableSpec& s) {
    auto view = FirstView(rows, s, "account row missing");
    if (!view.HasValue()) return core::Result<Account>::Fail(view.Err());
    const RowView& v = view.Value();
    Account e;
    e.account_id = AsU64(ViewInt(v, "account_id"));
    e.username = AsStr(ViewText(v, "username"));
    e.password_hash = AsStr(ViewText(v, "password_hash"));
    e.created_at = AsStr(ViewText(v, "created_at"));
    e.version = ViewVersion(v, s);
    return core::Result<Account>::Ok(std::move(e));
}

// ============================ Character ============================

core::Result<std::uint64_t> EntityCodec<Character>::KeyOf(const Character& e) {
    if (e.char_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "char_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.char_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Character>::ToRows(const Character& e,
                                                                     const TableSpec& s) {
    std::unordered_map<std::string, MySqlValue> m;
    m["char_id"] = MySqlValue::Uint(e.char_id);
    m["account_id"] = MySqlValue::Uint(e.account_id);
    m["name"] = MySqlValue::Text(e.name);
    m["level"] = MySqlValue::Uint(e.level);
    m["exp"] = MySqlValue::Uint(e.exp);
    m["attrs_json"] = MySqlValue::Text(e.attrs_json);
    m["version"] = MySqlValue::Uint(e.version);
    return OneRow(OrderParams(s, m));
}

core::Result<Character> EntityCodec<Character>::FromRows(const std::vector<MySqlRow>& rows,
                                                         const TableSpec& s) {
    auto view = FirstView(rows, s, "character row missing");
    if (!view.HasValue()) return core::Result<Character>::Fail(view.Err());
    const RowView& v = view.Value();
    Character e;
    e.char_id = AsU64(ViewInt(v, "char_id"));
    e.account_id = AsU64(ViewInt(v, "account_id"));
    e.name = AsStr(ViewText(v, "name"));
    e.level = AsU32(ViewInt(v, "level"));
    e.exp = AsU64(ViewInt(v, "exp"));
    e.attrs_json = AsStr(ViewText(v, "attrs_json"));
    e.version = ViewVersion(v, s);
    return core::Result<Character>::Ok(std::move(e));
}

// ============================ Inventory（多行） ============================

core::Result<std::uint64_t> EntityCodec<Inventory>::KeyOf(const Inventory& e) {
    if (e.char_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "inventory char_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.char_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Inventory>::ToRows(const Inventory& e,
                                                                     const TableSpec& s) {
    std::vector<MySqlParams> rows;
    rows.reserve(e.slots.size());
    for (const auto& slot : e.slots) {
        std::unordered_map<std::string, MySqlValue> m;
        m["char_id"] = MySqlValue::Uint(e.char_id);
        m["slot"] = MySqlValue::Uint(slot.slot);
        m["item_guid"] = MySqlValue::Uint(slot.item_guid);
        m["item_def_id"] = MySqlValue::Uint(slot.item_def_id);
        m["count"] = MySqlValue::Uint(slot.count);
        m["durability"] = MySqlValue::Uint(slot.durability);
        m["version"] = MySqlValue::Uint(slot.version);
        auto p = OrderParams(s, m);
        if (!p.HasValue()) return core::Result<std::vector<MySqlParams>>::Fail(p.Err());
        rows.push_back(std::move(p.Value()));
    }
    return MultiRows(std::move(rows));
}

core::Result<Inventory> EntityCodec<Inventory>::FromRows(const std::vector<MySqlRow>& rows,
                                                         const TableSpec& s) {
    Inventory e;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const RowView v = MakeRowView(s, rows[i]);
        if (v.empty()) {
            return core::Result<Inventory>::Fail(
                Err(core::ErrorCode::INTERNAL_ERROR, "inventory row shape mismatch"));
        }
        if (i == 0) e.char_id = AsU64(ViewInt(v, "char_id"));
        InventorySlot slot;
        slot.slot = AsU32(ViewInt(v, "slot"));
        slot.item_guid = AsU64(ViewInt(v, "item_guid"));
        slot.item_def_id = AsU32(ViewInt(v, "item_def_id"));
        slot.count = AsU32(ViewInt(v, "count"));
        slot.durability = AsU32(ViewInt(v, "durability"));
        slot.version = ViewVersion(v, s);
        e.slots.push_back(std::move(slot));
    }
    return core::Result<Inventory>::Ok(std::move(e));
}

// ============================ Equipment（多行） ============================

core::Result<std::uint64_t> EntityCodec<Equipment>::KeyOf(const Equipment& e) {
    if (e.char_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "equipment char_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.char_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Equipment>::ToRows(const Equipment& e,
                                                                     const TableSpec& s) {
    std::vector<MySqlParams> rows;
    rows.reserve(e.slots.size());
    for (const auto& slot : e.slots) {
        std::unordered_map<std::string, MySqlValue> m;
        m["char_id"] = MySqlValue::Uint(e.char_id);
        m["slot"] = MySqlValue::Uint(slot.slot);
        m["item_guid"] = MySqlValue::Uint(slot.item_guid);
        m["version"] = MySqlValue::Uint(slot.version);
        auto p = OrderParams(s, m);
        if (!p.HasValue()) return core::Result<std::vector<MySqlParams>>::Fail(p.Err());
        rows.push_back(std::move(p.Value()));
    }
    return MultiRows(std::move(rows));
}

core::Result<Equipment> EntityCodec<Equipment>::FromRows(const std::vector<MySqlRow>& rows,
                                                         const TableSpec& s) {
    Equipment e;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const RowView v = MakeRowView(s, rows[i]);
        if (v.empty()) {
            return core::Result<Equipment>::Fail(
                Err(core::ErrorCode::INTERNAL_ERROR, "equipment row shape mismatch"));
        }
        if (i == 0) e.char_id = AsU64(ViewInt(v, "char_id"));
        EquipmentSlot slot;
        slot.slot = AsU32(ViewInt(v, "slot"));
        slot.item_guid = AsU64(ViewInt(v, "item_guid"));
        slot.version = ViewVersion(v, s);
        e.slots.push_back(std::move(slot));
    }
    return core::Result<Equipment>::Ok(std::move(e));
}

// ============================ Quest（多行） ============================

core::Result<std::uint64_t> EntityCodec<Quest>::KeyOf(const Quest& e) {
    if (e.char_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "quest char_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.char_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Quest>::ToRows(const Quest& e,
                                                                 const TableSpec& s) {
    std::vector<MySqlParams> rows;
    rows.reserve(e.entries.size());
    for (const auto& entry : e.entries) {
        std::unordered_map<std::string, MySqlValue> m;
        m["char_id"] = MySqlValue::Uint(e.char_id);
        m["quest_id"] = MySqlValue::Uint(entry.quest_id);
        m["status"] = MySqlValue::Uint(entry.status);
        m["progress_json"] = MySqlValue::Text(entry.progress_json);
        m["version"] = MySqlValue::Uint(entry.version);
        auto p = OrderParams(s, m);
        if (!p.HasValue()) return core::Result<std::vector<MySqlParams>>::Fail(p.Err());
        rows.push_back(std::move(p.Value()));
    }
    return MultiRows(std::move(rows));
}

core::Result<Quest> EntityCodec<Quest>::FromRows(const std::vector<MySqlRow>& rows,
                                                 const TableSpec& s) {
    Quest e;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const RowView v = MakeRowView(s, rows[i]);
        if (v.empty()) {
            return core::Result<Quest>::Fail(
                Err(core::ErrorCode::INTERNAL_ERROR, "quest row shape mismatch"));
        }
        if (i == 0) e.char_id = AsU64(ViewInt(v, "char_id"));
        QuestEntry entry;
        entry.quest_id = AsU32(ViewInt(v, "quest_id"));
        entry.status = AsU32(ViewInt(v, "status"));
        entry.progress_json = AsStr(ViewText(v, "progress_json"));
        entry.version = ViewVersion(v, s);
        e.entries.push_back(std::move(entry));
    }
    return core::Result<Quest>::Ok(std::move(e));
}

// ============================ Guild ============================

core::Result<std::uint64_t> EntityCodec<Guild>::KeyOf(const Guild& e) {
    if (e.guild_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "guild_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.guild_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Guild>::ToRows(const Guild& e,
                                                                const TableSpec& s) {
    std::unordered_map<std::string, MySqlValue> m;
    m["guild_id"] = MySqlValue::Uint(e.guild_id);
    m["name"] = MySqlValue::Text(e.name);
    m["leader_id"] = MySqlValue::Uint(e.leader_id);
    m["member_count"] = MySqlValue::Uint(e.member_count);
    m["version"] = MySqlValue::Uint(e.version);
    return OneRow(OrderParams(s, m));
}

core::Result<Guild> EntityCodec<Guild>::FromRows(const std::vector<MySqlRow>& rows,
                                                 const TableSpec& s) {
    auto view = FirstView(rows, s, "guild row missing");
    if (!view.HasValue()) return core::Result<Guild>::Fail(view.Err());
    const RowView& v = view.Value();
    Guild e;
    e.guild_id = AsU64(ViewInt(v, "guild_id"));
    e.name = AsStr(ViewText(v, "name"));
    e.leader_id = AsU64(ViewInt(v, "leader_id"));
    e.member_count = AsU32(ViewInt(v, "member_count"));
    e.version = ViewVersion(v, s);
    return core::Result<Guild>::Ok(std::move(e));
}

// ============================ Mail ============================

core::Result<std::uint64_t> EntityCodec<Mail>::KeyOf(const Mail& e) {
    if (e.mail_id == 0) {
        return core::Result<std::uint64_t>::Fail(
            Err(core::ErrorCode::INVALID_ARGUMENT, "mail_id must not be 0"));
    }
    return core::Result<std::uint64_t>::Ok(e.mail_id);
}

core::Result<std::vector<MySqlParams>> EntityCodec<Mail>::ToRows(const Mail& e, const TableSpec& s) {
    std::unordered_map<std::string, MySqlValue> m;
    m["mail_id"] = MySqlValue::Uint(e.mail_id);
    m["receiver_id"] = MySqlValue::Uint(e.receiver_id);
    m["sender_id"] = MySqlValue::Uint(e.sender_id);
    m["payload"] = MySqlValue::Text(e.payload);
    m["status"] = MySqlValue::Uint(e.status);
    // expire_at 是 TIMESTAMP NULL：空串绑定到 TIMESTAMP 会触发 1292（非法日期），
    // 故空值必须显式绑定为 SQL NULL（「永不过期」语义）。
    m["expire_at"] = e.expire_at.empty() ? MySqlValue::Null() : MySqlValue::Text(e.expire_at);
    m["version"] = MySqlValue::Uint(e.version);
    return OneRow(OrderParams(s, m));
}

core::Result<Mail> EntityCodec<Mail>::FromRows(const std::vector<MySqlRow>& rows,
                                               const TableSpec& s) {
    auto view = FirstView(rows, s, "mail row missing");
    if (!view.HasValue()) return core::Result<Mail>::Fail(view.Err());
    const RowView& v = view.Value();
    Mail e;
    e.mail_id = AsU64(ViewInt(v, "mail_id"));
    e.receiver_id = AsU64(ViewInt(v, "receiver_id"));
    e.sender_id = AsU64(ViewInt(v, "sender_id"));
    e.payload = AsStr(ViewText(v, "payload"));
    e.status = AsU32(ViewInt(v, "status"));
    e.expire_at = AsStr(ViewText(v, "expire_at"));
    e.version = ViewVersion(v, s);
    return core::Result<Mail>::Ok(std::move(e));
}

// ============================ 工厂 ============================

Repositories MakeRepositories(MySqlStore& store) {
    return Repositories{AccountRepo(store, AccountTable()),
                        CharacterRepo(store, CharacterTable()),
                        InventoryRepo(store, InventoryTable()),
                        EquipmentRepo(store, EquipmentTable()),
                        QuestRepo(store, QuestTable()),
                        GuildRepo(store, GuildTable()),
                        MailRepo(store, MailTable())};
}

}  // namespace mmo::data::mysql
