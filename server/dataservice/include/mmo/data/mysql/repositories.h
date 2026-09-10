// server/dataservice/include/mmo/data/mysql/repositories.h
//
// TASK-028 · 7 张逻辑表的领域对象与仓储（§8 Data Model / §15.8）。
//
// 每张表 = 一个 TableSpec + 一个 EntityCodec 特化 + 一个类型别名。
// 新增表只需在本文件追加，不改动 repository.h / mysql_store.h（§27.4 扩展点）。
//
// 主键与分片键：
//   - 单行表：account(account_id) / character(char_id) / guild(guild_id) / mail(mail_id)
//   - 多行表：inventory(char_id, slot) / equipment(char_id, slot) / quest(char_id, quest_id)
//     复合主键的**首列恒为分片键**，故同一角色的全部子行必落同一分片（§9 单分片事务可得）。
//
// 通用列（§8）：每张表都含 version（乐观锁）+ updated_at。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/data/mysql/repository.h"

namespace mmo::data::mysql {

// ============================ 领域对象（对齐 §8 列） ============================

/// account 表。`password_hash` 只存 argon2id 编码串（§20.7 禁明文/弱哈希）。
struct Account {
    std::uint64_t account_id{0};
    std::string username;
    std::string password_hash;
    std::string created_at;  // TIMESTAMP 文本（"YYYY-MM-DD HH:MM:SS"）
    std::uint32_t version{0};
};

/// character 表。
struct Character {
    std::uint64_t char_id{0};
    std::uint64_t account_id{0};
    std::string name;
    std::uint32_t level{1};
    std::uint64_t exp{0};
    std::string attrs_json;  // 属性快照（业务层负责编解码）
    std::uint32_t version{0};
};

/// inventory 子行。
struct InventorySlot {
    std::uint32_t slot{0};
    std::uint64_t item_guid{0};
    std::uint32_t item_def_id{0};
    std::uint32_t count{0};
    std::uint32_t durability{0};
    std::uint32_t version{0};
};

/// inventory 聚合（一个角色的全部背包格；Put 时整体替换）。
struct Inventory {
    std::uint64_t char_id{0};
    std::vector<InventorySlot> slots;
};

/// equipment 子行。
struct EquipmentSlot {
    std::uint32_t slot{0};
    std::uint64_t item_guid{0};
    std::uint32_t version{0};
};

/// equipment 聚合。
struct Equipment {
    std::uint64_t char_id{0};
    std::vector<EquipmentSlot> slots;
};

/// quest 子行。
struct QuestEntry {
    std::uint32_t quest_id{0};
    std::uint32_t status{0};
    std::string progress_json;
    std::uint32_t version{0};
};

/// quest 聚合。
struct Quest {
    std::uint64_t char_id{0};
    std::vector<QuestEntry> entries;
};

/// guild 表。
struct Guild {
    std::uint64_t guild_id{0};
    std::string name;
    std::uint64_t leader_id{0};
    std::uint32_t member_count{0};
    std::uint32_t version{0};
};

/// mail 表。`expire_at` 为 TIMESTAMP 文本；`status` 0=未读 1=已读 2=已领取。
struct Mail {
    std::uint64_t mail_id{0};
    std::uint64_t receiver_id{0};
    std::uint64_t sender_id{0};
    std::string payload;
    std::uint32_t status{0};
    std::string expire_at;
    std::uint32_t version{0};
};

// ============================ TableSpec ============================

TableSpec AccountTable();    // account(account_id)
TableSpec CharacterTable();  // character(char_id)
TableSpec InventoryTable();  // inventory(char_id, slot)
TableSpec EquipmentTable();  // equipment(char_id, slot)
TableSpec QuestTable();      // quest(char_id, quest_id)
TableSpec GuildTable();      // guild(guild_id)
TableSpec MailTable();       // mail(mail_id)

// ============================ EntityCodec 特化 ============================

template <>
struct EntityCodec<Account> {
    static core::Result<std::uint64_t> KeyOf(const Account& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Account& e, const TableSpec& s);
    static core::Result<Account> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

template <>
struct EntityCodec<Character> {
    static core::Result<std::uint64_t> KeyOf(const Character& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Character& e, const TableSpec& s);
    static core::Result<Character> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

template <>
struct EntityCodec<Inventory> {
    static core::Result<std::uint64_t> KeyOf(const Inventory& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Inventory& e, const TableSpec& s);
    static core::Result<Inventory> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

template <>
struct EntityCodec<Equipment> {
    static core::Result<std::uint64_t> KeyOf(const Equipment& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Equipment& e, const TableSpec& s);
    static core::Result<Equipment> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

template <>
struct EntityCodec<Quest> {
    static core::Result<std::uint64_t> KeyOf(const Quest& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Quest& e, const TableSpec& s);
    static core::Result<Quest> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

template <>
struct EntityCodec<Guild> {
    static core::Result<std::uint64_t> KeyOf(const Guild& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Guild& e, const TableSpec& s);
    static core::Result<Guild> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

template <>
struct EntityCodec<Mail> {
    static core::Result<std::uint64_t> KeyOf(const Mail& e);
    static core::Result<std::vector<MySqlParams>> ToRows(const Mail& e, const TableSpec& s);
    static core::Result<Mail> FromRows(const std::vector<MySqlRow>& rows, const TableSpec& s);
};

// ============================ 仓储别名（§15.8 命名） ============================

using AccountRepo = MySqlRepository<Account>;
using CharacterRepo = MySqlRepository<Character>;
using InventoryRepo = MySqlRepository<Inventory>;
using EquipmentRepo = MySqlRepository<Equipment>;
using QuestRepo = MySqlRepository<Quest>;
using GuildRepo = MySqlRepository<Guild>;
using MailRepo = MySqlRepository<Mail>;

/// 一次性构造全部仓储（共用同一个 MySqlStore 的分片池）。
struct Repositories {
    AccountRepo account;
    CharacterRepo character;
    InventoryRepo inventory;
    EquipmentRepo equipment;
    QuestRepo quest;
    GuildRepo guild;
    MailRepo mail;
};

Repositories MakeRepositories(MySqlStore& store);

}  // namespace mmo::data::mysql
