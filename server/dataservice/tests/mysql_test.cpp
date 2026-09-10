// server/dataservice/tests/mysql_test.cpp
//
// TASK-028 · MySQL 适配器测试（§16 单元 / §17 集成 / §19 Failure / §20 验收）。
//
// 设计：纯逻辑单测（分片路由 / SQL 构造 / 迁移版本解析 / 字段映射 / 口令哈希 /
//       错误码映射 / 跨分片拒绝）不依赖真实实例，离线必跑；
//       标注 [mysql] 的集成用例在实例不可达时**明确 SKIP**并打印原因，禁止伪装通过（§20.8）。
//
// 输出经 test_print.h（禁止裸 cout/printf）。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "test_print.h"

#include "mmo/core/error/error_code.h"
#include "mmo/data/mysql/connection_pool.h"
#include "mmo/data/mysql/migration.h"
#include "mmo/data/mysql/mysql_client.h"
#include "mmo/data/mysql/mysql_store.h"
#include "mmo/data/mysql/password_hash.h"
#include "mmo/data/mysql/repositories.h"
#include "mmo/data/mysql/shard_router.h"
#include "mmo/data/mysql/sql_builder.h"

namespace {

using namespace mmo::data;
using namespace mmo::data::mysql;
namespace core = mmo::core;
using core::test::ErrorFmt;

int g_fail = 0;
int g_skip = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            ErrorFmt("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);  \
            ++g_fail;                                                           \
            return;                                                             \
        }                                                                       \
    } while (0)

// ---- 测试用实例参数（与 docker/mysql/docker-compose.yml 一致）----
// 端口可用 MMO_MYSQL_TEST_PORT 覆盖：用于在实例在线时**真实验证**「无实例明确 SKIP」
// 分支（§20.8「不得伪装通过」），而无需停掉正在服务的实例。
constexpr char kHost[] = "127.0.0.1";
constexpr char kUser[] = "root";

std::uint16_t TestPort() {
    const char* env = std::getenv("MMO_MYSQL_TEST_PORT");
    if (env != nullptr && env[0] != '\0') {
        const unsigned long v = std::strtoul(env, nullptr, 10);
        if (v > 0 && v <= 65535) return static_cast<std::uint16_t>(v);
    }
    return 3306;
}
constexpr char kDbPrefix[] = "mmo_test_shard";

ShardEndpoint TestEndpoint(std::uint32_t shard) {
    ShardEndpoint ep;
    ep.host = kHost;
    ep.port = TestPort();
    ep.user = kUser;
    ep.database = std::string(kDbPrefix) + std::to_string(shard);
    return ep;
}

MySqlConfig TestConfig(std::uint32_t shards) {
    MySqlConfig cfg;
    cfg.password_env = "";  // 本地验证实例 root 无密码
    cfg.pool_size_per_shard = 4;
    cfg.endpoints.clear();
    for (std::uint32_t i = 0; i < shards; ++i) cfg.endpoints.push_back(TestEndpoint(i));
    return cfg;
}

/// 实例是否可达（用引导连接探测，避免污染业务连接池计数）。
bool MySqlAvailable() {
    ShardEndpoint boot;
    boot.host = kHost;
    boot.port = TestPort();
    boot.user = kUser;
    MySqlConfig cfg;
    cfg.password_env = "";
    cfg.connect_timeout = core::DurationMs{500};
    auto conn = MySqlConnection::Open(boot, cfg, "");
    return conn.HasValue();
}

/// 建立测试用 store（1 分片，库名 mmo_test_shard0），并保证库存在。
/// 返回 nullptr 表示实例不可达（调用方 SKIP）。
std::unique_ptr<MySqlStore> MakeStore(MySqlConfig& cfg_out) {
    MySqlConfig cfg = TestConfig(1);
    if (!EnsureDatabaseExists(cfg.endpoints[0], cfg, "").HasValue()) return nullptr;
    auto store = MySqlStore::CreateSingle(cfg.endpoints[0], cfg);
    if (!store.HasValue()) return nullptr;
    cfg_out = cfg;
    return std::move(store).Value();
}

Record MakeRec(const std::string& key, std::uint32_t ver, const std::string& payload) {
    Record r;
    r.key = key;
    r.version = ver;
    r.payload = payload;
    r.updated_at = core::MonotonicClock::Point();
    return r;
}

/// 明确 SKIP（§20.8）：打印 `[mysql]` 标记 + 原因，便于 grep 确认真实跳过而非伪装通过。
void Skip(const char* name, const char* why) {
    ErrorFmt("SKIP [mysql] %s (%s)\n", name, why);
    ++g_skip;
}

/// 引导连接执行任意 SQL（**仅测试**用于清库/建表，保证集成用例可重复运行）。
bool BootExec(const std::string& database, const std::string& sql) {
    ShardEndpoint ep;
    ep.host = kHost;
    ep.port = TestPort();
    ep.user = kUser;
    ep.database = database;
    MySqlConfig cfg;
    cfg.password_env = "";
    cfg.connect_timeout = core::DurationMs{2000};
    cfg.query_timeout = core::DurationMs{10000};
    auto conn = MySqlConnection::Open(ep, cfg, "");
    if (!conn.HasValue()) return false;
    auto rs = std::move(conn).Value().Execute(sql);
    return rs.HasValue();
}

/// 保证库内 kv_store 存在（幂等；供直接用连接池的用例使用）。
void EnsureKvStore(const std::string& database) {
    auto ddl = BuildCreateKvStoreSql();
    if (ddl.HasValue()) (void)BootExec(database, ddl.Value());
}

/// 清掉指定前缀的行（前缀是测试内的固定字面量，不含用户输入）。
void CleanKeys(const std::string& database, const std::string& prefix) {
    (void)BootExec(database, "DELETE FROM kv_store WHERE k LIKE '" + prefix + "%'");
}

/// 各集成用例使用的库名（与 MakeStore / TestEndpoint 保持一致）。
std::string ShardDb(std::uint32_t shard) { return std::string(kDbPrefix) + std::to_string(shard); }

// ============================================================================
// §16 单元：分片路由
// ============================================================================

void test_shard_router_create_and_validate() {
    ShardConfig sc;
    sc.shard_count = 4;
    std::vector<ShardEndpoint> eps;
    for (std::uint32_t i = 0; i < 4; ++i) eps.push_back(TestEndpoint(i));
    auto r = ShardRouter::Create(sc, eps);
    CHECK(r.HasValue());
    CHECK(r.Value().ShardCount() == 4);

    // shard_count 与 endpoints 数量不一致 -> 明确错误
    ShardConfig bad;
    bad.shard_count = 8;
    CHECK(!ShardRouter::Create(bad, eps).HasValue());

    // shard_count = 0 -> 明确错误
    ShardConfig zero;
    zero.shard_count = 0;
    CHECK(!ShardRouter::Create(zero, {}).HasValue());

    // 空库名 -> 明确错误
    std::vector<ShardEndpoint> no_db = eps;
    no_db[0].database.clear();
    CHECK(!ShardRouter::Create(sc, no_db).HasValue());
}

void test_shard_router_boundaries() {
    ShardConfig sc;
    sc.shard_count = 8;
    std::vector<ShardEndpoint> eps;
    for (std::uint32_t i = 0; i < 8; ++i) eps.push_back(TestEndpoint(i));
    auto router = ShardRouter::Create(sc, eps);
    CHECK(router.HasValue());
    const ShardRouter& r = router.Value();

    // 边界（§16）：id=0 与 uint64 极大值
    CHECK(r.ShardOf(0) == 0);
    CHECK(r.ShardOf(7) == 7);
    CHECK(r.ShardOf(8) == 0);
    CHECK(r.ShardOf(UINT64_MAX) == static_cast<std::uint32_t>(UINT64_MAX % 8));
    // 分片数不是 2 的幂时仍按取模（避免「按位与」这类隐含硬编码）
    ShardConfig sc7;
    sc7.shard_count = 7;
    std::vector<ShardEndpoint> eps7;
    for (std::uint32_t i = 0; i < 7; ++i) eps7.push_back(TestEndpoint(i));
    auto r7 = ShardRouter::Create(sc7, eps7);
    CHECK(r7.HasValue());
    CHECK(r7.Value().ShardOf(10) == 3);
    CHECK(r7.Value().ShardOf(0) == 0);

    // 端点查询与越界诊断
    CHECK(r.IsValidShard(0) && r.IsValidShard(7) && !r.IsValidShard(8));
    CHECK(r.EndpointOf(3).database == "mmo_test_shard3");

    // 自定义分片函数越界也要被取模收敛
    ShardConfig custom;
    custom.shard_count = 4;
    std::vector<ShardEndpoint> eps4;
    for (std::uint32_t i = 0; i < 4; ++i) eps4.push_back(TestEndpoint(i));
    custom.shard_func = [](std::uint64_t) -> std::uint32_t { return 99; };
    auto rc = ShardRouter::Create(custom, eps4);
    CHECK(rc.HasValue());
    CHECK(rc.Value().ShardOf(1) == 99 % 4);
}

void test_parse_key() {
    auto a = ShardRouter::ParseKey("character:10086");
    CHECK(a.HasValue());
    CHECK(a.Value().domain == "character");
    CHECK(a.Value().entity_id == 10086);

    CHECK(!ShardRouter::ParseKey("no-colon").HasValue());
    CHECK(!ShardRouter::ParseKey(":5").HasValue());
    CHECK(!ShardRouter::ParseKey("character:").HasValue());
    CHECK(!ShardRouter::ParseKey("character:abc").HasValue());
    CHECK(!ShardRouter::ParseKey("character:12x").HasValue());
}

void test_reshard_is_explicitly_unimplemented() {
    ShardConfig sc;
    sc.shard_count = 4;
    std::vector<ShardEndpoint> eps;
    for (std::uint32_t i = 0; i < 4; ++i) eps.push_back(TestEndpoint(i));
    auto router = ShardRouter::Create(sc, eps);
    CHECK(router.HasValue());
    ShardRouter r = router.Value();

    ReshardPlan mismatch;
    mismatch.from_count = 4;
    mismatch.to_count = 9;  // 与 new_count 不一致
    CHECK(!r.Reshard(8, mismatch).HasValue());

    ReshardPlan ok_plan;
    ok_plan.from_count = 4;
    ok_plan.to_count = 8;
    // 第一版不执行迁移：必须**明确报错**，禁止静默成功（§7）
    CHECK(!r.Reshard(8, ok_plan).HasValue());
}

// ============================================================================
// §16 单元：SQL 构造（乐观锁）
// ============================================================================

void test_identifier_guard() {
    CHECK(IsSafeIdentifier("kv_store"));
    CHECK(IsSafeIdentifier("t_1"));
    CHECK(!IsSafeIdentifier(""));
    CHECK(!IsSafeIdentifier("1abc"));
    CHECK(!IsSafeIdentifier("a b"));
    CHECK(!IsSafeIdentifier("a;DROP TABLE x"));
    CHECK(!IsSafeIdentifier("a`b"));
    CHECK(!IsSafeIdentifier(std::string(65, 'a')));

    // 不安全标识符必须被拒（防止把外部输入拼进 SQL）
    CHECK(!BuildGuardedUpdateSql("bad name", "k").HasValue());
    CHECK(!BuildGuardedUpdateSql("ok_table", "bad;col").HasValue());
    CHECK(!BuildSelectSql("t", "").HasValue());
}

void test_save_mode_decision() {
    VersionCheck guard{5, true};
    CHECK(DecideSaveMode(guard) == SaveMode::UpdateGuarded);
    VersionCheck create{0, true};
    CHECK(DecideSaveMode(create) == SaveMode::Insert);
    VersionCheck blind{0, false};
    CHECK(DecideSaveMode(blind) == SaveMode::Upsert);
}

void test_optimistic_lock_sql() {
    auto upd = BuildGuardedUpdateSql("kv_store", "k");
    CHECK(upd.HasValue());
    CHECK(upd.Value() ==
          "UPDATE kv_store SET payload=?,version=?,updated_at=NOW() WHERE k=? AND version=?");
    CHECK(upd.Value().find("AND version=?") != std::string::npos);  // §20.4 乐观锁在 WHERE 里

    auto ups = BuildUpsertSql("kv_store", "k");
    CHECK(ups.HasValue());
    CHECK(ups.Value().find("ON DUPLICATE KEY UPDATE") != std::string::npos);

    auto ins = BuildInsertSql("kv_store", "k");
    CHECK(ins.HasValue());
    CHECK(ins.Value() == "INSERT INTO kv_store (k,payload,version,updated_at) VALUES (?,?,?,NOW())");

    auto del = BuildGuardedDeleteSql("kv_store", "k");
    CHECK(del.HasValue());
    CHECK(del.Value() == "DELETE FROM kv_store WHERE k=? AND version=?");

    auto pdel = BuildPlainDeleteSql("kv_store", "k");
    CHECK(pdel.HasValue());
    CHECK(pdel.Value() == "DELETE FROM kv_store WHERE k=?");

    auto sel = BuildSelectSql("kv_store", "k");
    CHECK(sel.HasValue());
    CHECK(sel.Value() == "SELECT payload,version FROM kv_store WHERE k=?");

    auto batch = BuildBatchSelectSql("kv_store", "k", 3);
    CHECK(batch.HasValue());
    CHECK(batch.Value() == "SELECT k,payload,version FROM kv_store WHERE k IN (?,?,?)");
    CHECK(!BuildBatchSelectSql("kv_store", "k", 0).HasValue());

    auto ddl = BuildCreateKvStoreSql();
    CHECK(ddl.HasValue());
    CHECK(ddl.Value().find("PRIMARY KEY (k)") != std::string::npos);
    auto mig = BuildCreateSchemaMigrationsSql();
    CHECK(mig.HasValue());
    CHECK(mig.Value().find("PRIMARY KEY (version)") != std::string::npos);
    CHECK(BuildSelectMigrationsSql().Value().find("ORDER BY version") != std::string::npos);
}

// ============================================================================
// §16 单元：Repository 字段映射（脱离数据库）
// ============================================================================

MySqlRow RowOf(const std::vector<std::string>& cols) {
    MySqlRow r;
    for (const auto& c : cols) {
        if (c == "\x01NULL") {
            r.values.push_back(MySqlValue::Null());
        } else {
            r.values.push_back(MySqlValue::Text(c));
        }
    }
    return r;
}

void test_repository_column_mapping() {
    const TableSpec spec = CharacterTable();
    CHECK(spec.table == "character");
    CHECK(spec.key_column == "char_id");
    CHECK(!spec.is_multi_row());

    // SELECT 投影顺序必须与 spec.columns 一致
    auto sql = BuildRepoSelectSql(spec);
    CHECK(sql.HasValue());
    CHECK(sql.Value() ==
          "SELECT `char_id`,`account_id`,`name`,`level`,`exp`,`attrs_json`,`version`"
          " FROM `character` WHERE `char_id`=?");

    // 行 -> 列名视图 -> 领域对象
    const std::vector<MySqlRow> rows{RowOf({"1001", "77", "Aria", "12", "3400", "{}", "3"})};
    auto ch = EntityCodec<Character>::FromRows(rows, spec);
    CHECK(ch.HasValue());
    CHECK(ch.Value().char_id == 1001);
    CHECK(ch.Value().account_id == 77);
    CHECK(ch.Value().name == "Aria");
    CHECK(ch.Value().level == 12);
    CHECK(ch.Value().exp == 3400);
    CHECK(ch.Value().version == 3);

    // NULL 列不崩溃（按缺省值处理）
    const std::vector<MySqlRow> with_null{RowOf({"1", "2", "Bob", "1", "0", "\x01NULL", "0"})};
    auto ch2 = EntityCodec<Character>::FromRows(with_null, spec);
    CHECK(ch2.HasValue());
    CHECK(ch2.Value().attrs_json.empty());

    // 列数不匹配 -> 明确错误（不静默错位）
    const std::vector<MySqlRow> short_row{RowOf({"1", "2"})};
    CHECK(!EntityCodec<Character>::FromRows(short_row, spec).HasValue());

    // 空结果 -> NOT_FOUND（调用方按 nullopt 处理，此处只是编解码层）
    CHECK(!EntityCodec<Character>::FromRows({}, spec).HasValue());

    // 参数顺序与 spec.columns 一致
    Character c;
    c.char_id = 5;
    c.account_id = 9;
    c.name = "Zed";
    c.level = 3;
    c.exp = 10;
    c.attrs_json = "{\"hp\":1}";
    c.version = 2;
    auto params = EntityCodec<Character>::ToRows(c, spec);
    CHECK(params.HasValue());
    CHECK(params.Value().size() == 1);
    CHECK(params.Value()[0].size() == 7);
    CHECK(params.Value()[0][0].i64 == 5);
    CHECK(params.Value()[0][2].str == "Zed");

    // key 缺省 -> 明确错误
    Character zero;
    zero.char_id = 0;
    CHECK(!EntityCodec<Character>::KeyOf(zero).HasValue());

    // 多行表：聚合编解码 + ORDER BY 子键 + INSERT 列顺序
    const TableSpec inv = InventoryTable();
    CHECK(inv.is_multi_row());
    CHECK(BuildRepoSelectSql(inv).Value().find("ORDER BY `slot`") != std::string::npos);
    Inventory bag;
    bag.char_id = 42;
    bag.slots.push_back(InventorySlot{1, 900, 5, 3, 100, 1});
    bag.slots.push_back(InventorySlot{2, 901, 6, 1, 50, 2});
    auto inv_rows = EntityCodec<Inventory>::ToRows(bag, inv);
    CHECK(inv_rows.HasValue());
    CHECK(inv_rows.Value().size() == 2);
    CHECK(inv_rows.Value()[0].size() == 7);
    auto back = EntityCodec<Inventory>::FromRows(
        {RowOf({"42", "1", "900", "5", "3", "100", "1"}),
         RowOf({"42", "2", "901", "6", "1", "50", "2"})},
        inv);
    CHECK(back.HasValue());
    CHECK(back.Value().char_id == 42);
    CHECK(back.Value().slots.size() == 2);
    CHECK(back.Value().slots[1].item_guid == 901);
    CHECK(back.Value().slots[1].count == 1);

    // 多行表没有单行 guarded UPDATE
    CHECK(!BuildRepoGuardedUpdateSql(inv).HasValue());
    // 单行表 guarded UPDATE 的 SET 列表不含 key 列，参数必须同步裁剪
    auto guarded = BuildRepoGuardedUpdateSql(spec);
    CHECK(guarded.HasValue());
    CHECK(guarded.Value().find("SET `account_id`=?") != std::string::npos);
    CHECK(guarded.Value().find("SET `char_id`=?") == std::string::npos);
    auto trimmed = WithoutKeyColumn(spec, params.Value()[0]);
    CHECK(trimmed.size() == 6);

    // 缺列的映射必须报错（禁止静默补 NULL）
    std::unordered_map<std::string, MySqlValue> partial;
    partial["char_id"] = MySqlValue::Uint(1);
    CHECK(!OrderParams(spec, partial).HasValue());
}

void test_repository_specs_match_ddl_columns() {
    // 7 张表的 spec 必须齐全且都含乐观锁列（§8 / §20.4）
    const std::vector<TableSpec> specs{AccountTable(),  CharacterTable(), InventoryTable(),
                                       EquipmentTable(), QuestTable(),     GuildTable(),
                                       MailTable()};
    CHECK(specs.size() == 7);
    for (const auto& s : specs) {
        CHECK(!s.table.empty());
        CHECK(!s.key_column.empty());
        CHECK(s.version_column == "version");
        bool has_key = false;
        bool has_version = false;
        for (const auto& c : s.columns) {
            if (c == s.key_column) has_key = true;
            if (c == "version") has_version = true;
        }
        CHECK(has_key);
        CHECK(has_version);
        CHECK(BuildRepoInsertSql(s).HasValue());
        CHECK(BuildRepoSelectSql(s).HasValue());
        CHECK(BuildRepoDeleteByKeySql(s).HasValue());
        CHECK(MySqlStore::IsKnownTable(s.table));
    }
    // 多行表的复合主键首列必须是分片键
    CHECK(InventoryTable().key_column == "char_id");
    CHECK(InventoryTable().sub_key_column == "slot");
    CHECK(QuestTable().sub_key_column == "quest_id");
}

// ============================================================================
// §16 单元：迁移版本解析 + SQL 切分
// ============================================================================

void test_migration_parse_version() {
    CHECK(MigrationRunner::ParseVersion("001_init.sql").Value() == 1);
    CHECK(MigrationRunner::ParseVersion("002_mail_expire_index.sql").Value() == 2);
    CHECK(MigrationRunner::ParseVersion("123_x.sql").Value() == 123);
    CHECK(!MigrationRunner::ParseVersion("init.sql").HasValue());
    CHECK(!MigrationRunner::ParseVersion("001_init.txt").HasValue());
    CHECK(!MigrationRunner::ParseVersion("abc_init.sql").HasValue());
    CHECK(!MigrationRunner::ParseVersion("000_zero.sql").HasValue());  // 版本 0 非法
    CHECK(!MigrationRunner::ParseVersion("_init.sql").HasValue());
}

void test_migration_split_statements() {
    const std::string sql =
        "-- 注释行\n"
        "CREATE TABLE a (x INT);\n"
        "/* 块注释; 里也有分号 */\n"
        "CREATE TABLE b (y VARCHAR(8) DEFAULT ';');\n"
        "\n"
        ";\n"
        "INSERT INTO b VALUES ('a;b');\n";
    const auto stmts = MigrationRunner::SplitStatements(sql);
    CHECK(stmts.size() == 3);
    CHECK(stmts[0] == "CREATE TABLE a (x INT)");
    CHECK(stmts[1].find("DEFAULT ';'") != std::string::npos);  // 字面量内分号不被切
    CHECK(stmts[2].find("'a;b'") != std::string::npos);
    CHECK(MigrationRunner::SplitStatements("   \n  ").empty());
}

// ============================================================================
// §16 单元：口令 salted hash（禁明文）
// ============================================================================

void test_password_hash() {
    auto h = HashPassword("P@ssw0rd-中文");
    CHECK(h.HasValue());
    CHECK(IsArgon2idEncoded(h.Value()));
    CHECK(h.Value().find("$argon2id$") == 0);
    // 同一个口令两次哈希必须不同（盐随机）
    auto h2 = HashPassword("P@ssw0rd-中文");
    CHECK(h2.HasValue());
    CHECK(h.Value() != h2.Value());
    // 校验
    CHECK(VerifyPassword("P@ssw0rd-中文", h.Value()));
    CHECK(!VerifyPassword("wrong", h.Value()));
    CHECK(!VerifyPassword("", h.Value()));
    // 非法编码串不得崩溃、不得误判通过
    CHECK(!VerifyPassword("x", ""));
    CHECK(!VerifyPassword("x", "plaintext"));
    CHECK(!VerifyPassword("x", "$argon2id$broken"));
    CHECK(!IsArgon2idEncoded("md5:5f4dcc3b5aa765d61d8327deb882cf99"));

    // 空口令与过短盐被拒
    CHECK(!HashPassword("").HasValue());
    PasswordHashOptions bad;
    bad.salt_bytes = 4;
    CHECK(!HashPassword("x", bad).HasValue());

    // 账号编解码拒绝明文口令入库（§20.7）
    Account acc;
    acc.account_id = 1;
    acc.username = "u";
    acc.password_hash = "plaintext-password";
    CHECK(!EntityCodec<Account>::ToRows(acc, AccountTable()).HasValue());
    acc.password_hash = h.Value();
    CHECK(EntityCodec<Account>::ToRows(acc, AccountTable()).HasValue());
}

// ============================================================================
// §16/§19 单元：原生错误码映射 + 跨分片拒绝
// ============================================================================

void test_error_code_mapping() {
    CHECK(IsDeadlockCode(1213));   // ER_LOCK_DEADLOCK
    CHECK(IsDeadlockCode(1205));   // ER_LOCK_WAIT_TIMEOUT
    CHECK(!IsDeadlockCode(1062));
    CHECK(IsConnectionLostCode(2002));
    CHECK(IsConnectionLostCode(2003));
    CHECK(IsConnectionLostCode(2006));
    CHECK(IsConnectionLostCode(2055));
    CHECK(!IsConnectionLostCode(1213));

    CHECK(MapMySqlError(1213) == core::ErrorCode::BUSY);
    CHECK(MapMySqlError(1205) == core::ErrorCode::BUSY);
    CHECK(MapMySqlError(2013) == core::ErrorCode::TIMEOUT);       // 读超时
    CHECK(MapMySqlError(2006) == core::ErrorCode::BUSY);          // 服务端掉线
    CHECK(MapMySqlError(1062) == core::ErrorCode::VERSION_CONFLICT);  // 重复键
    CHECK(MapMySqlError(1045) == core::ErrorCode::UNAUTHORIZED);
    CHECK(MapMySqlError(99999) == core::ErrorCode::INTERNAL_ERROR);
    // 可重试性自洽：BUSY/TIMEOUT 都是可重试
    CHECK(core::IsRetryable(MapMySqlError(1213)));
    CHECK(core::IsRetryable(MapMySqlError(2013)));
}

void test_require_single_shard() {
    ShardConfig sc;
    sc.shard_count = 4;
    std::vector<ShardEndpoint> eps;
    for (std::uint32_t i = 0; i < 4; ++i) eps.push_back(TestEndpoint(i));
    auto router = ShardRouter::Create(sc, eps);
    CHECK(router.HasValue());

    // id 0 与 4 落同一分片（0 % 4 == 4 % 4）
    const std::vector<DataKey> same{"character:0", "inventory:4"};
    CHECK(RequireSingleShard(router.Value(), same).Value() == 0);

    // 跨分片 -> 明确拒绝（§20.5 / §21）
    const std::vector<DataKey> cross{"character:1", "character:2"};
    auto r = RequireSingleShard(router.Value(), cross);
    CHECK(!r.HasValue());
    CHECK(r.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);

    // 空集合与非法键
    CHECK(!RequireSingleShard(router.Value(), {}).HasValue());
    const std::vector<DataKey> bad{"no-colon"};
    CHECK(!RequireSingleShard(router.Value(), bad).HasValue());
}

// ============================================================================
// §19 Failure：实例不可达（不崩溃、明确错误）
// ============================================================================

void test_create_fails_when_down() {
    ShardEndpoint ep = TestEndpoint(0);
    ep.port = 3399;  // 必死端口
    MySqlConfig cfg = TestConfig(1);
    cfg.endpoints[0] = ep;
    cfg.connect_timeout = core::DurationMs{500};

    auto pool = MySqlConnectionPool::Create(ep, cfg);
    CHECK(!pool.HasValue());
    CHECK(pool.Err().Code() == core::ErrorCode::BUSY);

    auto store = MySqlStore::CreateSingle(ep, cfg);
    CHECK(!store.HasValue());
    CHECK(store.Err().Code() == core::ErrorCode::BUSY);

    // 口令 env 名给了但变量未设置 -> 明确失败（禁止空密码兜底）
    ShardEndpoint ep2 = TestEndpoint(0);
    MySqlConfig cfg2 = TestConfig(1);
    cfg2.password_env = "MMORPG_MYSQL_PASSWORD_NOT_SET_XYZ";
    auto pool2 = MySqlConnectionPool::Create(ep2, cfg2);
    CHECK(!pool2.HasValue());
    CHECK(pool2.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);
}

void test_pool_stats_defaults() {
    // 未连接时的默认指标：全 0，不得伪造
    const MySqlPoolStats s{};
    CHECK(s.in_use == 0 && s.idle == 0 && s.wait_count == 0);
    CHECK(s.acquire_timeout_count == 0 && s.reconnect_count == 0);
    CHECK(s.op_count == 0 && s.slow_query_count == 0 && s.deadlock_count == 0);
}

// ============================================================================
// §17 集成（需真实 MySQL；不可达时明确 SKIP）
// ============================================================================

void test_integration_crud_1000() {
    if (!MySqlAvailable()) {
        Skip("test_integration_crud_1000", "no mysql instance");
        return;
    }
    MySqlConfig cfg;
    CleanKeys(ShardDb(0), "character:100");  // 保证用例可重复运行
    auto store = MakeStore(cfg);
    CHECK(store != nullptr);
    MySqlStore& s = *store;

    constexpr int kRows = 1000;
    // 写入 1000 条角色数据（Insert 路径：默认 VersionCheck{} 表示「期望不存在」）
    std::vector<std::uint64_t> ids;
    ids.reserve(kRows);
    for (int i = 0; i < kRows; ++i) {
        const std::uint64_t id = 100000 + static_cast<std::uint64_t>(i);
        auto r = s.Save(MakeRec("character:" + std::to_string(id), 1,
                                "payload-" + std::to_string(i)));
        if (!r.HasValue()) {
            CHECK(r.HasValue());
        }
        ids.push_back(id);
    }

    // 单条读取
    auto one = s.Load("character:100000");
    CHECK(one.HasValue() && one.Value().has_value());
    CHECK(one.Value()->payload == "payload-0");
    CHECK(one.Value()->version == 1);

    // 批量读取（按分片分组 + IN 查询）
    std::vector<DataKey> keys;
    keys.reserve(kRows);
    for (const auto id : ids) keys.push_back("character:" + std::to_string(id));
    auto batch = s.BatchLoad(keys);
    CHECK(batch.HasValue());
    CHECK(batch.Value().size() == static_cast<std::size_t>(kRows));

    // 缺失键静默跳过（不报错）
    std::vector<DataKey> missing{"character:999999999"};
    auto none = s.BatchLoad(missing);
    CHECK(none.HasValue() && none.Value().empty());

    // 覆盖写：默认 VersionCheck{} = {expected=0, required=true} 表示「期望不存在」，
    // 与 TASK-026 InMemoryStore 冻结语义一致 -> 对已存在的键必须 VERSION_CONFLICT
    auto dup_default = s.Save(MakeRec("character:100000", 2, "payload-0-v2"));
    CHECK(!dup_default.HasValue());
    CHECK(dup_default.Err().Code() == core::ErrorCode::VERSION_CONFLICT);

    // 基于已读版本 1 的 guarded update -> 成功
    auto rewrite = s.Save(MakeRec("character:100000", 2, "payload-0-v2"), VersionCheck{1, true});
    CHECK(rewrite.HasValue());
    auto after = s.Load("character:100000");
    CHECK(after.Value()->payload == "payload-0-v2");
    CHECK(after.Value()->version == 2);
    CHECK(s.Delete("character:100000").HasValue());
    auto gone = s.Load("character:100000");
    CHECK(gone.HasValue() && !gone.Value().has_value());

    // 清理
    for (const auto id : ids) (void)s.Delete("character:" + std::to_string(id));
}

void test_integration_optimistic_lock_conflict() {
    if (!MySqlAvailable()) {
        Skip("test_integration_optimistic_lock_conflict", "no mysql instance");
        return;
    }
    MySqlConfig cfg;
    CleanKeys(ShardDb(0), "character:200001");
    auto store = MakeStore(cfg);
    CHECK(store != nullptr);
    MySqlStore& s = *store;

    const std::string key = "character:200001";
    // 期望插入（expected=0, required）-> 首次成功
    CHECK(s.Save(MakeRec(key, 1, "v1"), VersionCheck{0, true}).HasValue());

    // 两个写者都基于 version=1 做 guarded update：第一个成功、第二个 VERSION_CONFLICT
    auto w1 = s.Save(MakeRec(key, 2, "w1"), VersionCheck{1, true});
    CHECK(w1.HasValue());
    auto w2 = s.Save(MakeRec(key, 2, "w2"), VersionCheck{1, true});
    CHECK(!w2.HasValue());
    CHECK(w2.Err().Code() == core::ErrorCode::VERSION_CONFLICT);

    auto cur = s.Load(key);
    CHECK(cur.HasValue() && cur.Value().has_value());
    CHECK(cur.Value()->payload == "w1");   // 后写者未覆盖
    CHECK(cur.Value()->version == 2);

    // 重复插入（expected=0 但已存在）-> 1062 -> VERSION_CONFLICT
    auto dup = s.Save(MakeRec(key, 1, "again"), VersionCheck{0, true});
    CHECK(!dup.HasValue());
    CHECK(dup.Err().Code() == core::ErrorCode::VERSION_CONFLICT);

    // 带版本的删除同样受保护
    auto del_bad = s.Delete(key, VersionCheck{99, true});
    CHECK(!del_bad.HasValue());
    CHECK(del_bad.Err().Code() == core::ErrorCode::VERSION_CONFLICT);
    CHECK(s.Delete(key, VersionCheck{2, true}).HasValue());
}

void test_integration_transaction_rollback() {
    if (!MySqlAvailable()) {
        Skip("test_integration_transaction_rollback", "no mysql instance");
        return;
    }
    MySqlConfig cfg;
    CleanKeys(ShardDb(0), "character:3000");
    auto store = MakeStore(cfg);
    CHECK(store != nullptr);
    MySqlStore& s = *store;

    const std::string key = "character:300001";
    CHECK(s.Save(MakeRec(key, 1, "committed")).HasValue());

    // 手工事务：写入后回滚 -> 数据不变（§17 事务回滚验证）
    {
        auto lease = s.AcquireShard(0);
        CHECK(lease.HasValue());
        auto* conn = lease.Value().handle();
        CHECK(conn->Begin().HasValue());
        auto w = conn->ExecuteParams("INSERT INTO kv_store (k,payload,version) VALUES (?,?,1)"
                                     " ON DUPLICATE KEY UPDATE payload=VALUES(payload)",
                                     MySqlParams{MySqlValue::Text(key), MySqlValue::Blob("rolled-back")});
        CHECK(w.HasValue());
        CHECK(conn->Rollback().HasValue());
    }  // lease 析构即归还（作用域退出前不得提前销毁，否则二次析构）

    auto after = s.Load(key);
    CHECK(after.HasValue() && after.Value().has_value());
    CHECK(after.Value()->payload == "committed");

    // BatchSaveAtomic：同分片多条一起提交（单分片 store，必然同分片）
    std::vector<Record> batch{MakeRec("character:300002", 0, "a"),
                              MakeRec("character:300006", 0, "b")};
    CHECK(s.BatchSaveAtomic(batch).HasValue());
    CHECK(s.Load("character:300002").Value().has_value());
    CHECK(s.Load("character:300006").Value().has_value());

    // 原子批量带版本校验：基于已读版本 0 提交 -> 成功；版本不符 -> 整批回滚
    std::vector<Record> guarded{MakeRec("character:300002", 0, "a2"),
                                MakeRec("character:300006", 0, "b2")};
    CHECK(s.BatchSaveAtomic(guarded).HasValue());  // version=0 -> 覆盖
    std::vector<Record> stale{MakeRec("character:300002", 7, "a3")};  // 期望版本 7，实际 0
    auto stale_r = s.BatchSaveAtomic(stale);
    CHECK(!stale_r.HasValue());
    CHECK(stale_r.Err().Code() == core::ErrorCode::VERSION_CONFLICT);
    CHECK(s.Load("character:300002").Value()->payload == "a2");  // 回滚后未被改写

    // 跨分片批量必须明确拒绝（§20.5）——单分片 store 构造不出跨分片，故放到多分片用例
    // test_integration_multi_shard_routing 中验证。

    (void)s.Delete(key);
    for (const auto& r : batch) (void)s.Delete(r.key);
}

void test_integration_batch_save_outcomes() {
    if (!MySqlAvailable()) {
        Skip("test_integration_batch_save_outcomes", "no mysql instance");
        return;
    }
    MySqlConfig cfg;
    auto store = MakeStore(cfg);
    CHECK(store != nullptr);
    MySqlStore& s = *store;

    // BatchSave 逐条语义（TASK-026 冻结）：version==0 -> 不校验直接覆盖；
    // version!=0 -> 期望实际版本 == version。此处用 0 保证可重复运行。
    std::vector<Record> recs;
    for (int i = 0; i < 50; ++i) {
        recs.push_back(MakeRec("inventory:" + std::to_string(400000 + i), 0, "b"));
    }
    auto out = s.BatchSave(recs);
    CHECK(out.HasValue());
    CHECK(out.Value().size() == 50);
    for (const auto& o : out.Value()) {
        CHECK(o.ok);  // 全部成功
    }
    // 逐条结算：键与输入一一对应
    CHECK(out.Value()[0].key == recs[0].key);
    CHECK(out.Value()[49].key == recs[49].key);
    for (const auto& r : recs) (void)s.Delete(r.key);
}

void test_integration_migration() {
    if (!MySqlAvailable()) {
        Skip("test_integration_migration", "no mysql instance");
        return;
    }
    // 专属库，避免与 CRUD 用例互相影响；先删库保证「本次全部新应用」可断言
    ShardEndpoint ep = TestEndpoint(7);
    ep.database = "mmo_test_migrate";
    (void)BootExec("", "DROP DATABASE IF EXISTS mmo_test_migrate");
    MySqlConfig cfg;
    cfg.password_env = "";
    cfg.connect_timeout = core::DurationMs{1000};
    cfg.query_timeout = core::DurationMs{5000};
    CHECK(EnsureDatabaseExists(ep, cfg, "").HasValue());

    auto runner = MigrationRunner::Create(ep, cfg, "");
    CHECK(runner.HasValue());
    auto& m = runner.Value();

    auto applied = m.Up("database/migrations");
    CHECK(applied.HasValue());
    CHECK(applied.Value().size() >= 2);  // 至少 001 + 002
    CHECK(applied.Value()[0] == 1);

    auto status = m.Status("database/migrations");
    CHECK(status.HasValue());
    CHECK(status.Value().size() >= 2);
    for (const auto& st : status.Value()) {
        CHECK(st.applied);
        CHECK(!st.failed);
    }

    // 重跑幂等：无新增版本
    auto again = m.Up("database/migrations");
    CHECK(again.HasValue());
    CHECK(again.Value().empty());

    // dry-run 不写库：pending=0
    auto dry = m.DryRun("database/migrations");
    CHECK(dry.HasValue());
    CHECK(dry.Value().find("pending_versions=0") != std::string::npos);

    // 迁移记录表存在且 success=1
    CHECK(m.RegistryExists().Value());

    // 7 张逻辑表 + kv_store 全部建表成功（§17 / §20.2）
    ShardEndpoint ep2 = ep;
    auto conn = MySqlConnection::Open(ep2, cfg, "");
    CHECK(conn.HasValue());
    auto tables = std::move(conn).Value().Execute("SELECT table_name FROM information_schema.tables"
                                       " WHERE table_schema = DATABASE() ORDER BY table_name");
    CHECK(tables.HasValue());
    std::string joined;
    for (const auto& row : tables.Value().rows) {
        const auto t = row.Text(0);
        if (t.has_value()) joined += *t + ",";
    }
    for (const char* want : {"account", "character", "inventory", "equipment", "quest", "guild",
                             "mail", "kv_store", "schema_migrations"}) {
        CHECK(joined.find(std::string(want) + ",") != std::string::npos);
    }
}

void test_integration_repository_roundtrip() {
    if (!MySqlAvailable()) {
        Skip("test_integration_repository_roundtrip", "no mysql instance");
        return;
    }
    MySqlConfig cfg = TestConfig(1);
    ShardEndpoint ep = TestEndpoint(0);
    ep.database = "mmo_test_migrate";  // 用已迁移过 7 张表的库
    cfg.endpoints = {ep};

    auto store = MySqlStore::CreateSingle(ep, cfg);
    CHECK(store.HasValue());
    MySqlStore& s = *store.Value();
    auto repos = MakeRepositories(s);

    // account（口令必须是 salted hash）
    auto hash = HashPassword("hunter2");
    CHECK(hash.HasValue());
    Account acc;
    acc.account_id = 900001;
    acc.username = "aria";
    acc.password_hash = hash.Value();
    acc.created_at = "2026-01-01 00:00:00";
    acc.version = 0;
    (void)repos.account.Remove(acc.account_id);
    CHECK(repos.account.Put(acc).HasValue());
    auto acc_back = repos.account.GetById(acc.account_id);
    CHECK(acc_back.HasValue() && acc_back.Value().has_value());
    CHECK(acc_back.Value()->username == "aria");
    CHECK(VerifyPassword("hunter2", acc_back.Value()->password_hash));
    CHECK(!VerifyPassword("wrong", acc_back.Value()->password_hash));

    // character：guarded update（乐观锁）
    Character ch;
    ch.char_id = 900002;
    ch.account_id = acc.account_id;
    ch.name = "Aria";
    ch.level = 10;
    ch.exp = 500;
    ch.attrs_json = "{\"str\":10}";
    ch.version = 1;
    (void)repos.character.Remove(ch.char_id);
    CHECK(repos.character.Put(ch).HasValue());
    auto ch_back = repos.character.GetById(ch.char_id);
    CHECK(ch_back.HasValue() && ch_back.Value().has_value());
    CHECK(ch_back.Value()->level == 10);
    CHECK(ch_back.Value()->version == 1);

    ch.version = 2;
    CHECK(repos.character.Put(ch, VersionCheck{1, true}).HasValue());
    ch.level = 11;
    auto conflict = repos.character.Put(ch, VersionCheck{1, true});
    CHECK(!conflict.HasValue());
    CHECK(conflict.Err().Code() == core::ErrorCode::VERSION_CONFLICT);

    // inventory（多行聚合：整体替换 + 回读）
    Inventory bag;
    bag.char_id = 900002;
    bag.slots.push_back(InventorySlot{1, 7001, 101, 5, 100, 1});
    bag.slots.push_back(InventorySlot{2, 7002, 102, 1, 90, 1});
    CHECK(repos.inventory.Put(bag).HasValue());
    auto bag_back = repos.inventory.GetById(bag.char_id);
    CHECK(bag_back.HasValue() && bag_back.Value().has_value());
    CHECK(bag_back.Value()->slots.size() == 2);
    CHECK(bag_back.Value()->slots[0].item_guid == 7001);

    // 再次 Put 只留 1 格 -> 旧子行被替换（事务内 delete + insert）
    bag.slots.resize(1);
    CHECK(repos.inventory.Put(bag).HasValue());
    auto bag2 = repos.inventory.GetById(bag.char_id);
    CHECK(bag2.Value()->slots.size() == 1);

    // quest / equipment / guild / mail 基本往返
    Quest q;
    q.char_id = 900002;
    q.entries.push_back(QuestEntry{5, 1, "{\"k\":1}", 1});
    CHECK(repos.quest.Put(q).HasValue());
    auto q_back = repos.quest.GetById(q.char_id);
    CHECK(q_back.HasValue() && q_back.Value()->entries.size() == 1);
    CHECK(q_back.Value()->entries[0].quest_id == 5);

    Equipment eq;
    eq.char_id = 900002;
    eq.slots.push_back(EquipmentSlot{3, 8001, 1});
    CHECK(repos.equipment.Put(eq).HasValue());
    auto eq_back = repos.equipment.GetById(eq.char_id);
    CHECK(eq_back.HasValue() && eq_back.Value()->slots.size() == 1);

    Guild g;
    g.guild_id = 900003;
    g.name = "TestGuild";
    g.leader_id = 900002;
    g.member_count = 1;
    (void)repos.guild.Remove(g.guild_id);
    CHECK(repos.guild.Put(g).HasValue());
    auto g_back = repos.guild.GetById(g.guild_id);
    CHECK(g_back.HasValue() && g_back.Value()->name == "TestGuild");

    Mail mail;
    mail.mail_id = 900004;
    mail.receiver_id = 900002;
    mail.sender_id = 0;
    mail.payload = "welcome";
    mail.status = 0;
    mail.expire_at = "";
    (void)repos.mail.Remove(mail.mail_id);
    CHECK(repos.mail.Put(mail).HasValue());
    auto m_back = repos.mail.GetById(mail.mail_id);
    CHECK(m_back.HasValue() && m_back.Value()->payload == "welcome");

    // 清理
    CHECK(repos.inventory.Remove(bag.char_id).HasValue());
    CHECK(repos.quest.Remove(q.char_id).HasValue());
    CHECK(repos.equipment.Remove(eq.char_id).HasValue());
    CHECK(repos.character.Remove(ch.char_id).HasValue());
    CHECK(repos.account.Remove(acc.account_id).HasValue());
    CHECK(repos.guild.Remove(g.guild_id).HasValue());
    CHECK(repos.mail.Remove(mail.mail_id).HasValue());
}

void test_integration_pool_exhaustion() {
    if (!MySqlAvailable()) {
        Skip("test_integration_pool_exhaustion", "no mysql instance");
        return;
    }
    ShardEndpoint ep = TestEndpoint(0);
    MySqlConfig cfg = TestConfig(1);
    cfg.pool_size_per_shard = 1;
    auto pool = MySqlConnectionPool::Create(ep, cfg);
    CHECK(pool.HasValue());

    {
        auto held = pool.Value()->Acquire(core::DurationMs{500});
        CHECK(held.HasValue());
        auto second = pool.Value()->Acquire(core::DurationMs{50});
        CHECK(!second.HasValue());
        CHECK(second.Err().Code() == core::ErrorCode::BUSY);  // 池耗尽 -> BUSY
        CHECK(pool.Value()->Stats().acquire_timeout_count >= 1);
        CHECK(pool.Value()->Stats().in_use == 1);
    }  // held 归还

    auto third = pool.Value()->Acquire(core::DurationMs{500});
    CHECK(third.HasValue());  // 归还后可复用，未死锁
    CHECK(pool.Value() != nullptr);
}

void test_integration_slow_query_and_timeout() {
    if (!MySqlAvailable()) {
        Skip("test_integration_slow_query_and_timeout", "no mysql instance");
        return;
    }
    ShardEndpoint ep = TestEndpoint(0);
    MySqlConfig cfg = TestConfig(1);
    cfg.query_timeout = core::DurationMs{1000};      // 读超时 1s
    cfg.slow_query_threshold = core::DurationMs{50};  // 50ms 以上记慢查询
    auto pool = MySqlConnectionPool::Create(ep, cfg);
    CHECK(pool.HasValue());

    std::atomic<int> slow_hits{0};
    std::atomic<std::uint64_t> slow_ns{0};
    pool.Value()->SetSlowQueryHook([&](std::string_view, std::uint64_t ns) {
        slow_hits.fetch_add(1);
        slow_ns.store(ns);
    });

    auto lease = pool.Value()->Acquire(core::DurationMs{500});
    CHECK(lease.HasValue());
    MySqlConnection* conn = lease.Value().handle();

    // 慢查询：SLEEP(0.2) > 阈值 -> slow_query_count 增加且 hook 触发
    const auto t0 = std::chrono::steady_clock::now();
    auto slow = conn->Execute("SELECT SLEEP(0.2)");
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
    CHECK(slow.HasValue());
    pool.Value()->NoteStatement("SELECT SLEEP(0.2)", ns);
    CHECK(pool.Value()->Stats().slow_query_count >= 1);
    CHECK(slow_hits.load() >= 1);
    CHECK(slow_ns.load() > 0);

    // 超过读超时 -> TIMEOUT（不是挂死）
    const auto t1 = std::chrono::steady_clock::now();
    auto hang = conn->Execute("SELECT SLEEP(3)");
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t1).count();
    CHECK(!hang.HasValue());
    CHECK(hang.Err().Code() == core::ErrorCode::TIMEOUT);
    CHECK(elapsed_ms < 2500);              // 1s 超时生效，未等到 3s
    CHECK(conn->broken());                 // 超时连接被标记损坏 -> 归还时丢弃
}

void test_integration_lock_wait_maps_to_busy() {
    if (!MySqlAvailable()) {
        Skip("test_integration_lock_wait_maps_to_busy", "no mysql instance");
        return;
    }
    ShardEndpoint ep = TestEndpoint(0);
    MySqlConfig cfg = TestConfig(1);
    cfg.query_timeout = core::DurationMs{5000};  // 读超时放宽，让锁等待先超时
    EnsureKvStore(ep.database);  // 本用例直接用连接池，需自行保证表已建
    CleanKeys(ep.database, "character:500001");
    auto pool = MySqlConnectionPool::Create(ep, cfg);
    CHECK(pool.HasValue());

    const std::string key = "character:500001";

    {
        auto a = pool.Value()->Acquire(core::DurationMs{500});
        auto b = pool.Value()->Acquire(core::DurationMs{500});
        CHECK(a.HasValue() && b.HasValue());
        MySqlConnection* ca = a.Value().handle();
        MySqlConnection* cb = b.Value().handle();

        CHECK(ca->Begin().HasValue());
        CHECK(ca->ExecuteParams("INSERT INTO kv_store (k,payload,version) VALUES (?,?,1)"
                                " ON DUPLICATE KEY UPDATE payload=VALUES(payload)",
                                MySqlParams{MySqlValue::Text(key), MySqlValue::Blob("lock-holder")})
                  .HasValue());

        // 第二个会话把锁等待超时压到 1s：拿到 1205（而非读超时）
        (void)cb->Execute("SET SESSION innodb_lock_wait_timeout = 1");
        CHECK(cb->Begin().HasValue());
        const auto t0 = std::chrono::steady_clock::now();
        auto blocked = cb->ExecuteParams("UPDATE kv_store SET payload=? WHERE k=?",
                                         MySqlParams{MySqlValue::Blob("blocked"),
                                                     MySqlValue::Text(key)});
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(!blocked.HasValue());
        CHECK(ms < 4000);  // 1s 锁等待超时生效（或读超时先到）
        CHECK(core::IsRetryable(blocked.Err().Code()));  // 锁等待/超时都必须可重试

        (void)cb->Rollback();
        (void)ca->Rollback();
    }  // 两个 lease 各自析构归还（禁止显式析构，避免二次析构）

    auto store = MySqlStore::CreateSingle(ep, cfg);
    CHECK(store.HasValue());
    (void)store.Value()->Delete(key);
}

/// §17 / §19：实例侧杀掉连接 -> 明确错误（不崩溃/不挂死）-> 池丢弃坏连接 -> 自动重连成功。
void test_integration_pool_reconnects_after_kill() {
    if (!MySqlAvailable()) {
        Skip("test_integration_pool_reconnects_after_kill", "no mysql instance");
        return;
    }
    ShardEndpoint ep = TestEndpoint(0);
    MySqlConfig cfg = TestConfig(1);
    cfg.pool_size_per_shard = 1;                      // 池里只有一条，便于观察丢弃与重建
    cfg.query_timeout = core::DurationMs{2000};
    EnsureKvStore(ep.database);
    auto pool = MySqlConnectionPool::Create(ep, cfg);
    CHECK(pool.HasValue());

    // 先借出一次拿到服务端 connection id（归还后池里就是这一条连接）
    std::uint64_t victim = 0;
    {
        auto lease = pool.Value()->Acquire(core::DurationMs{500});
        CHECK(lease.HasValue());
        auto rs = lease.Value().handle()->Execute("SELECT CONNECTION_ID()");
        CHECK(rs.HasValue());
        const auto id = rs.Value().rows.front().Int(0);
        CHECK(id.has_value());
        victim = static_cast<std::uint64_t>(*id);
    }
    CHECK(victim != 0);

    // 模拟实例侧断连：用引导连接 KILL 掉池中的那条连接
    CHECK(BootExec("", "KILL " + std::to_string(victim)));
    const std::uint64_t reconnects_before = pool.Value()->Stats().reconnect_count;

    // 借出的是已死连接：使用必须返回明确错误，且被标记损坏（供归还时丢弃）
    {
        auto lease = pool.Value()->Acquire(core::DurationMs{500});
        CHECK(lease.HasValue());
        auto rs = lease.Value().handle()->Execute("SELECT 1");
        CHECK(!rs.HasValue());
        // 实例侧 kill 后客户端表现为 CR_SERVER_LOST(2013) -> 映射 TIMEOUT（可重试）；
        // 若连接器改报 2006/2003 等则为 BUSY。二者都是「后端不可用」类，必须可重试（§19）。
        CHECK(rs.Err().Code() == core::ErrorCode::TIMEOUT ||
              rs.Err().Code() == core::ErrorCode::BUSY);
        CHECK(core::IsRetryable(rs.Err().Code()));
        CHECK(lease.Value().handle()->broken());
    }  // 归还 -> 丢弃坏连接并累加 reconnect_count

    // 下次借出必须自动重建，并可正常读写（「可重连」）
    auto again = pool.Value()->Acquire(core::DurationMs{2000});
    CHECK(again.HasValue());
    auto ok = again.Value().handle()->Execute("SELECT 1");
    CHECK(ok.HasValue());
    CHECK(pool.Value()->Stats().reconnect_count > reconnects_before);
    CHECK(pool.Value()->Health() != HealthStatus::Unavailable);
}

void test_integration_multi_shard_routing() {
    if (!MySqlAvailable()) {
        Skip("test_integration_multi_shard_routing", "no mysql instance");
        return;
    }
    // 4 分片：同一业务 ID 必落同一分片，不同分片各自独立库
    MySqlConfig cfg = TestConfig(4);
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (!EnsureDatabaseExists(cfg.endpoints[i], cfg, "").HasValue()) {
            Skip("test_integration_multi_shard_routing", "cannot create shard db");
            return;
        }
        CleanKeys(ShardDb(i), "character:");  // 保证用例可重复运行
    }
    ShardConfig sc;
    sc.shard_count = 4;
    auto store = MySqlStore::Create(sc, cfg);
    CHECK(store.HasValue());
    MySqlStore& s = *store.Value();

    // 路由：id 0 与 8 都落分片 0（0 % 4 == 8 % 4）；键不同则是**不同行**，
    // 只有完全相同的 DataKey 才会互相覆盖。
    CHECK(s.Save(MakeRec("character:8", 1, "eight")).HasValue());
    CHECK(s.Save(MakeRec("character:0", 1, "zero")).HasValue());
    CHECK(s.router().ShardOf(0) == s.router().ShardOf(8));
    auto v8 = s.Load("character:8");
    auto v0 = s.Load("character:0");
    CHECK(v8.HasValue() && v8.Value().has_value());
    CHECK(v0.HasValue() && v0.Value().has_value());
    CHECK(v8.Value()->payload == "eight");
    CHECK(v0.Value()->payload == "zero");

    // 同键覆盖必须带版本（默认 VersionCheck{} 期望不存在 -> 冲突）
    auto dup = s.Save(MakeRec("character:8", 2, "eight-v2"));
    CHECK(!dup.HasValue());
    CHECK(dup.Err().Code() == core::ErrorCode::VERSION_CONFLICT);
    CHECK(s.Save(MakeRec("character:8", 2, "eight-v2"), VersionCheck{1, true}).HasValue());
    CHECK(s.Load("character:8").Value()->version == 2);

    CHECK(s.Health(0) == HealthStatus::Healthy);
    CHECK(s.Health(3) == HealthStatus::Healthy);
    CHECK(s.Health(9) == HealthStatus::Unavailable);  // 越界分片不崩溃
    CHECK(s.pool_stats(0).op_count > 0);

    // 跨分片原子批量必须被明确拒绝（§20.5），且不得留下任何一侧的写入
    std::vector<Record> cross{MakeRec("character:1", 1, "x"),   // 1 % 4 == 1
                              MakeRec("character:2", 1, "y")};  // 2 % 4 == 2
    auto bad = s.BatchSaveAtomic(cross);
    CHECK(!bad.HasValue());
    CHECK(bad.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);
    CHECK(!s.Load("character:1").Value().has_value());
    CHECK(!s.Load("character:2").Value().has_value());

    (void)s.Delete("character:0");
    (void)s.Delete("character:8");
}

}  // namespace

int main() {
    // ---- §16 / §19 单元（离线必跑）----
    test_shard_router_create_and_validate();
    test_shard_router_boundaries();
    test_parse_key();
    test_reshard_is_explicitly_unimplemented();
    test_identifier_guard();
    test_save_mode_decision();
    test_optimistic_lock_sql();
    test_repository_column_mapping();
    test_repository_specs_match_ddl_columns();
    test_migration_parse_version();
    test_migration_split_statements();
    test_password_hash();
    test_error_code_mapping();
    test_require_single_shard();
    test_create_fails_when_down();
    test_pool_stats_defaults();

    // ---- §17 / §19 集成（需真实 MySQL，否则明确 SKIP）----
    test_integration_migration();
    test_integration_crud_1000();
    test_integration_optimistic_lock_conflict();
    test_integration_transaction_rollback();
    test_integration_batch_save_outcomes();
    test_integration_repository_roundtrip();
    test_integration_pool_exhaustion();
    test_integration_slow_query_and_timeout();
    test_integration_lock_wait_maps_to_busy();
    test_integration_pool_reconnects_after_kill();
    test_integration_multi_shard_routing();

    if (g_skip > 0) {
        ErrorFmt("DataService.MySql: %d SKIPPED (no mysql instance)\n", g_skip);
    }
    if (g_fail == 0) {
        ErrorFmt("DataService.MySql.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("DataService.MySql.Suite: %d FAIL\n", g_fail);
    return g_fail;
}
