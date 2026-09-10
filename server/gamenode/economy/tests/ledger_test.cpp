// server/gamenode/economy/tests/ledger_test.cpp — TASK-030 §16 / §17 / §19
//
// 覆盖：
//   §16 单元：SHA-256 FIPS 已知向量；LEntry 十六字段与自洽校验；规范化哈希的
//             字段敏感性；幂等四状态机全路径（含 TTL 过期与注入故障）；Ledger 的
//             Append / Flush / 批量落库 / VerifyChain / QueryByPlayer / 重复拒绝 /
//             队列背压 / 大条目直写；内存足迹断言。
//   §17 集成：1 万次混合经济操作 → Σ账本净额 == Σ余额变化、资金守恒、哈希链完整、
//             幂等表无悬挂 InFlight；并落盘 CSV 供 tools/audit/economy_audit.py 对账。
//   §19 故障：**五场景**（重复请求 / RPC 重试 / 断线重连 / GameNode Crash / 数据库重试）
//             + 装备发放重复（不复制 ItemGuid），每个场景都断言「余额或物品只变 1 次」。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "sha256.h"  // 内部头（白盒：FIPS 已知向量）—— 仅本模块测试可 include
#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/economy/economy_command.h"
#include "mmo/game/economy/economy_system.h"
#include "mmo/game/economy/ledger/idempotency_store.h"
#include "mmo/game/economy/ledger/ledger.h"
#include "mmo/game/economy/ledger/ledger_store.h"
#include "mmo/game/economy/price_table.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/inventory/inventory.h"
#include "mmo/game/inventory/inventory_system.h"
#include "mmo/game/inventory/item.h"
#include "mmo/game/role/exp_curve.h"
#include "mmo/game/role/persistence_adapter.h"
#include "mmo/game/role/role_system.h"
#include "mmo/game/scene/scene_context.h"

namespace {

namespace core = mmo::core;
using namespace mmo::game;
using namespace mmo::game::economy;
using namespace mmo::game::inventory;
using namespace mmo::game::economy::ledger;

using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
using LEntry = mmo::game::economy::ledger::LedgerEntry;

using Sha256 = mmo::game::economy::ledger::detail::Sha256;

int g_fail = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_fail;                                                     \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                              \
    do {                                                                            \
        ++g_checks;                                                                 \
        const auto _a = (a);                                                        \
        const auto _b = (b);                                                        \
        if (!(_a == _b)) {                                                          \
            ErrorFmt("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b);         \
            ++g_fail;                                                               \
        }                                                                           \
    } while (0)

// ------------------------------------------------------------------ 环境

/// 与本模块其它测试一致的场景环境（唯一差别：经济系统按场景单独装配）。
struct Env {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
    EntityManager mgr;
    SceneContext ctx;
    role::InMemoryPersistenceAdapter sink;
    role::ExpCurve curve;
    role::RoleSystem role;
    ItemDefStore defs;
    InventorySystem inv;
    PriceTable prices;

    static role::ExpCurve LoadCurve() {
        auto r = role::ExpCurve::LoadFromFile("config/gameplay/exp_curve.json");
        if (!r.HasValue()) {
            ErrorFmt("FATAL: cannot load config/gameplay/exp_curve.json\n");
            std::exit(1);
        }
        return std::move(r).Value();
    }

    Env()
        : mgr(&bus),
          ctx(7, SceneType::World, 1, core::MonotonicClock::Point(), 0, mgr, bus, scheduler, arena),
          curve(LoadCurve()),
          role(sink, curve),
          inv(defs, role) {
        (void)defs.LoadDir("config/gameplay/items");
        (void)prices.LoadFromFile("config/gameplay/economy/prices.json");
        role.BindEventBus(bus);
        inv.BindEventBus(bus);
    }

    void EnsureChar(PlayerId p) { (void)role.LoadOrCreate(p, 1000u + p, ctx); }

    void Drain() {
        while (bus.QueueDepth() > 0) (void)bus.Drain();
    }

    /// 背包装某个 def_id 的总数量（物品守恒断言用）。
    std::uint32_t CountItems(PlayerId p, ItemId def_id) const {
        const Inventory* v = inv.View(p);
        if (v == nullptr) return 0;
        std::uint32_t total = 0;
        for (std::size_t i = 0; i < kMaxInventorySlots; ++i) {
            const ItemStack* s = v->At(static_cast<SlotIndex>(i));
            if (s != nullptr && s->guid != kInvalidItemGuid && s->def_id == def_id) {
                total += s->count;
            }
        }
        return total;
    }
};

std::uint64_t g_txn = 1'000'000;

/// §17 集成测试的落盘路径（供 tools/audit/economy_audit.py 二次对账）。
struct DumpPaths {
    const char* ledger_csv;
    const char* balances_csv;
    const char* meta_txt;
};

EconomyCommand Cmd(EconomyOp op, PlayerId p, const std::string& key, std::int64_t amount = 0,
                   CurrencyType cur = kCurrencyGold) {
    EconomyCommand c;
    ++g_txn;
    c.request_id = g_txn;
    c.trace_id = g_txn;
    c.transaction_id = g_txn;
    c.idempotency_key = key;
    c.player = p;
    c.op = op;
    c.amount = amount;
    c.currency = cur;
    c.reason = "test";
    c.source = "test:ledger";
    c.timestamp_ms = 1'700'000'000'000 + static_cast<std::int64_t>(g_txn % 1'000'000);
    return c;
}

std::string Hex(const std::array<std::uint8_t, 32>& d) {
    char buf[65];
    FormatHash(d, buf);
    return std::string(buf);
}

// ------------------------------------------------------------------ §16 SHA-256

void TestSha256KnownVectors() {
    Line("-- §16 SHA-256 FIPS 180-4 已知向量\n");
    // 向量来自 FIPS 180-4 / NIST 示例；这三条覆盖：单块、空输入、跨块（>55 字节）。
    CHECK_EQ(Hex(Sha256::Of({})),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(Hex(Sha256::Of(std::span<const std::uint8_t>(
                   reinterpret_cast<const std::uint8_t*>("abc"), 3))),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    const char* kTwoBlock = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_EQ(Hex(Sha256::Of(std::span<const std::uint8_t>(
                   reinterpret_cast<const std::uint8_t*>(kTwoBlock), 56))),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    // 流式分块必须与一次性一致（账本链是流式喂入的，分块边界不能改变结果）。
    Sha256 h;
    for (int i = 0; i < 56; ++i) {
        h.Update(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(kTwoBlock + i), 1));
    }
    CHECK_EQ(Hex(h.Final()), Hex(Sha256::Of(std::span<const std::uint8_t>(
                                    reinterpret_cast<const std::uint8_t*>(kTwoBlock), 56))));
}

// ------------------------------------------------------------------ §16 账本条目 / 哈希

void TestLEntryHash() {
    Line("-- §16 LedgerEntry 字段序列与哈希链\n");
    CHECK_EQ(CanonicalFieldOrder().size(), static_cast<std::size_t>(16));

    LEntry e;
    e.transaction_id = 42;
    e.request_id = 4242;
    e.idempotency_key = "purchase:1:3:1:7";
    e.player = 7;
    e.peer = 0;
    e.op = EconomyOp::Purchase;
    e.currency = kCurrencyGold;
    e.delta = -50;
    e.balance_after = 950;
    e.reason = "shop";
    e.source = "shop:3";
    e.timestamp_ms = 1'700'000'000'000;
    e.version = 9;
    e.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});

    // 结构性校验
    CHECK(ValidateLedgerEntry(e).HasValue());
    {
        LEntry bad = e;
        bad.transaction_id = kInvalidTransactionId;
        CHECK(!ValidateLedgerEntry(bad).HasValue());
        bad = e;
        bad.idempotency_key.clear();
        CHECK(!ValidateLedgerEntry(bad).HasValue());
        bad = e;
        bad.player = 0;
        CHECK(!ValidateLedgerEntry(bad).HasValue());
        bad = e;
        bad.op = static_cast<EconomyOp>(200);
        CHECK(!ValidateLedgerEntry(bad).HasValue());
    }

    const Hash256 h0 = ComputeEntryHash(kZeroHash, e);
    CHECK(!HashEqual(h0, kZeroHash));
    CHECK(HashEqual(h0, ComputeEntryHash(kZeroHash, e)));  // 确定性

    // **字段敏感性**：任一字段变化都必须改变摘要（否则链形同虚设）。
    {
        LEntry t = e;
        t.balance_after += 1;
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        t = e;
        t.delta += 1;
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        t = e;
        t.peer = 99;
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        t = e;
        t.op = EconomyOp::Reward;
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        t = e;
        t.item_deltas[0].count = 2;
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        t = e;
        t.reason = "shoP";
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        t = e;
        t.timestamp_ms += 1;
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, t), h0));
        // prev_hash 变化也必须改变摘要（链式传递）
        Hash256 other = h0;
        other[0] ^= 0xffu;
        CHECK(!HashEqual(ComputeEntryHash(other, e), h0));
    }

    // 长度前缀：`ab` + `c` 与 `a` + `bc` 必须得到不同摘要（否则字段边界歧义 → 可构造碰撞）
    {
        LEntry a = e;
        LEntry b = e;
        a.reason = "ab";
        a.source = "c";
        b.reason = "a";
        b.source = "bc";
        CHECK(!HashEqual(ComputeEntryHash(kZeroHash, a), ComputeEntryHash(kZeroHash, b)));
    }

    // VerifyEntryHash：篡改后必须为 false
    e.prev_hash = kZeroHash;
    e.hash = ComputeEntryHash(kZeroHash, e);
    CHECK(VerifyEntryHash(e));
    e.balance_after += 100;
    CHECK(!VerifyEntryHash(e));
}

// ------------------------------------------------------------------ §16 幂等状态机

void TestIdempotencyStateMachine() {
    Line("-- §16 幂等四状态机 / TTL / 注入故障\n");
    InMemoryIdemTable table;
    IdempotencyStore store(table);
    const DurationMs ttl{1000};

    // Fresh → InFlight
    auto s = store.TryBegin("k1", ttl);
    CHECK(s.HasValue());
    CHECK_EQ(static_cast<int>(s.Value()), static_cast<int>(IdemStatus::Fresh));

    // InFlight：重复请求返回 BUSY（不重复执行）
    s = store.TryBegin("k1", ttl);
    CHECK(s.HasValue());
    CHECK_EQ(static_cast<int>(s.Value()), static_cast<int>(IdemStatus::InFlight));
    CHECK_EQ(store.BusyCount(), static_cast<std::size_t>(1));

    // InFlight 期间的 Lookup 必须为空（没有首次结果可返回）
    auto look = store.Lookup("k1");
    CHECK(look.HasValue());
    CHECK(!look.Value().has_value());

    // Commit → Completed；重复请求返回首次结果
    EconomyResult first;
    first.applied = true;
    first.balance_after = 1234;
    first.version = 5;
    CHECK(store.Commit("k1", first).HasValue());
    s = store.TryBegin("k1", ttl);
    CHECK(s.HasValue());
    CHECK_EQ(static_cast<int>(s.Value()), static_cast<int>(IdemStatus::Completed));
    look = store.Lookup("k1");
    CHECK(look.HasValue() && look.Value().has_value());
    CHECK_EQ(look.Value()->balance_after, static_cast<std::int64_t>(1234));

    // Abort：删除行 → 回到 Fresh（业务拒绝不得毒化 key）
    CHECK(store.Abort("k2").HasValue());  // 不存在的 key 也算成功
    s = store.TryBegin("k2", ttl);
    CHECK(s.HasValue() && s.Value() == IdemStatus::Fresh);
    CHECK(store.Abort("k2").HasValue());
    s = store.TryBegin("k2", ttl);
    CHECK(s.HasValue() && s.Value() == IdemStatus::Fresh);

    // TTL 过期 → Failed（未决），且**不删除**（否则丢失「是否已落账」的判据）
    store.SetInjectedNowMs(1'000'000);
    CHECK(store.TryBegin("k3", ttl).Value() == IdemStatus::Fresh);
    s = store.TryBegin("k3", ttl);
    CHECK(s.HasValue() && s.Value() == IdemStatus::InFlight);  // 未过期
    store.SetInjectedNowMs(1'000'000 + 1001);
    s = store.TryBegin("k3", ttl);
    CHECK(s.HasValue() && s.Value() == IdemStatus::Failed);
    CHECK_EQ(store.ReapedCount(), static_cast<std::size_t>(1));
    // Failed 状态在同一 TTL 窗口内保持（必须由调用方查账本决定，不能自动重做）
    s = store.TryBegin("k3", ttl);
    CHECK(s.HasValue() && s.Value() == IdemStatus::Failed);
    CHECK(store.Abort("k3").HasValue());
    store.ClearInjectedNowMs();

    // 参数校验：无 TTL / 空 key 一律拒绝（§21）
    CHECK(!store.TryBegin("k4", DurationMs{0}).HasValue());
    CHECK(!store.TryBegin("", ttl).HasValue());
    // 状态流转前置条件：未占位就 Commit → NOT_FOUND
    CHECK(!store.Commit("never-begun", first).HasValue());
    CHECK(!store.Lookup("never-begun").Value().has_value());

    // 注入瞬时失败：TryBegin 如实返回 TIMEOUT（可重试），不产生半状态
    table.FailNextInserts(1);
    auto bad = store.TryBegin("k5", ttl);
    CHECK(!bad.HasValue());
    CHECK_EQ(static_cast<int>(bad.Err().Code()), static_cast<int>(core::ErrorCode::TIMEOUT));
    CHECK(!store.Lookup("k5").Value().has_value());
    CHECK(store.TryBegin("k5", ttl).HasValue());  // 故障消失后可正常占位

    // 并发（跨进程）等价场景：两个 store 实例共享同一张表
    InMemoryIdemTable shared;
    IdempotencyStore a(shared);
    IdempotencyStore b(shared);
    CHECK(a.TryBegin("race", ttl).Value() == IdemStatus::Fresh);
    CHECK(b.TryBegin("race", ttl).Value() == IdemStatus::InFlight);  // B 必须拿到 InFlight
    CHECK_EQ(b.BusyCount(), static_cast<std::size_t>(1));
}

// ------------------------------------------------------------------ §16 Ledger

void TestLedgerAppendFlushVerify() {
    Line("-- §16 Ledger Append / Flush / VerifyChain / 背压 / 直写\n");
    InMemoryLedgerStore store;
    Ledger ledger(store, LedgerConfig{64, 3});

    auto make = [](std::uint64_t i, PlayerId p, const std::string& key, std::int64_t delta) {
        LEntry e;
        e.transaction_id = 10'000 + i;
        e.request_id = 20'000 + i;
        e.idempotency_key = key;
        e.player = p;
        e.op = EconomyOp::AddCurrency;
        e.currency = kCurrencyGold;
        e.delta = delta;
        e.balance_after = 1000 + delta;
        e.reason = "unit";
        e.source = "unit:ledger";
        e.timestamp_ms = 1'700'000'000'000 + static_cast<std::int64_t>(i);
        return e;
    };

    // 结构性非法 → 拒绝且不入队
    {
        LEntry e = make(0, 100, "bad-key", 10);
        e.idempotency_key.clear();
        CHECK(!ledger.Append(e).HasValue());
        CHECK_EQ(ledger.PendingCount(), static_cast<std::size_t>(0));
    }

    for (std::uint64_t i = 0; i < 10; ++i) {
        CHECK(ledger.Append(make(i, 100 + static_cast<PlayerId>(i % 3), "k-" + std::to_string(i), 10))
                  .HasValue());
    }
    CHECK_EQ(ledger.PendingCount(), static_cast<std::size_t>(10));
    CHECK_EQ(store.Size(), static_cast<std::size_t>(0));  // 未 Flush = 未落库

    // 批次内重复 key → 应用层拒绝（UNIQUE 的第一道防线）
    {
        LEntry dup = make(99, 100, "k-0", 10);
        auto r = ledger.Append(dup);
        CHECK(!r.HasValue());
        CHECK_EQ(static_cast<int>(r.Err().Code()),
                 static_cast<int>(core::ErrorCode::VERSION_CONFLICT));
        CHECK_EQ(ledger.Stats().duplicates_rejected, static_cast<std::uint64_t>(1));
    }

    CHECK(ledger.Flush().HasValue());
    CHECK_EQ(store.Size(), static_cast<std::size_t>(10));
    CHECK_EQ(ledger.PendingCount(), static_cast<std::size_t>(0));

    // 全量链校验：必须连接且自洽
    auto ok = ledger.VerifyChain(0, 9'999'999'999'999LL);
    CHECK(ok.HasValue() && ok.Value());

    // 篡改检测：拿一份落库副本改一个字段 → 链校验必须失败（§20.3）
    {
        auto rows = store.LoadRange(0, 9'999'999'999'999LL);
        CHECK(rows.HasValue() && rows.Value().size() == 10);
        std::vector<LEntry> tampered = rows.Value();
        CHECK(VerifyChainOf(tampered, kZeroHash).Value());
        tampered[4].balance_after += 1;
        CHECK(!VerifyChainOf(tampered, kZeroHash).Value());
        // 只改 hash 也会被发现
        std::vector<LEntry> tampered2 = rows.Value();
        tampered2[2].hash[7] ^= 0x01u;
        CHECK(!VerifyChainOf(tampered2, kZeroHash).Value());
    }

    // 第二批继续接链（跨 Flush 批次仍是一条链）
    for (std::uint64_t i = 10; i < 20; ++i) {
        CHECK(ledger.Append(make(i, 100, "k-" + std::to_string(i), 10)).HasValue());
    }
    CHECK(ledger.Flush().HasValue());
    CHECK_EQ(store.Size(), static_cast<std::size_t>(20));
    CHECK(ledger.VerifyChain(0, 9'999'999'999'999LL).Value());

    // QueryByPlayer / QueryByKey
    {
        auto rows = ledger.QueryByPlayer(100, 0, 9'999'999'999'999LL);
        CHECK(rows.HasValue() && !rows.Value().empty());
        for (const LEntry& e : rows.Value()) CHECK(e.player == 100);
        auto one = ledger.QueryByKey("k-3");
        CHECK(one.HasValue() && one.Value().has_value());
        CHECK_EQ(one.Value()->player, static_cast<PlayerId>(100));
        auto missing = ledger.QueryByKey("nope");
        CHECK(missing.HasValue() && !missing.Value().has_value());
    }

    // 大条目直写（物品明细 > 4 条）必须保持顺序 → 链仍然成立
    {
        LEntry big = make(50, 777, "big-1", 0);
        for (std::uint32_t i = 0; i < 7; ++i) {
            big.item_deltas.push_back(ItemDelta{static_cast<ItemId>(i + 1), 1, kInvalidItemGuid});
        }
        CHECK(ledger.Append(big).HasValue());
        CHECK_EQ(ledger.Stats().big_entries_direct, static_cast<std::uint64_t>(1));
        CHECK_EQ(ledger.PendingCount(), static_cast<std::size_t>(0));  // 直写不留待落库
        CHECK_EQ(store.Size(), static_cast<std::size_t>(21));
        CHECK(ledger.VerifyChain(0, 9'999'999'999'999LL).Value());
    }

    // 队列背压：容量 2 + 落库持续失败 → 第 3 条必须 BUSY，禁止静默丢弃
    {
        InMemoryLedgerStore blocked;
        Ledger small(blocked, LedgerConfig{2, 1});
        CHECK(small.Append(make(60, 1, "s-1", 5)).HasValue());
        CHECK(small.Append(make(61, 1, "s-2", 5)).HasValue());
        blocked.FailNextAppends(1000);
        auto r = small.Append(make(62, 1, "s-3", 5));
        CHECK(!r.HasValue());
        CHECK_EQ(static_cast<int>(r.Err().Code()), static_cast<int>(core::ErrorCode::BUSY));
        CHECK_EQ(small.PendingCount(), static_cast<std::size_t>(2));  // 已入队的没丢
        // 落库恢复后可以继续
        blocked.FailNextAppends(0);
        CHECK(small.Flush().HasValue());
        CHECK_EQ(blocked.Size(), static_cast<std::size_t>(2));
        CHECK(small.Append(make(63, 1, "s-4", 5)).HasValue());
        CHECK(small.Flush().HasValue());
        CHECK_EQ(blocked.Size(), static_cast<std::size_t>(3));
        CHECK(small.VerifyChain(0, 9'999'999'999'999LL).Value());
    }

    // 内存足迹（§22 指标同口径）：槽 + 索引 + 变长实测均值
    CHECK(ledger.MemoryBytesPerEntry() <= 256u);
}

// ------------------------------------------------------------------ §19 五场景

void TestFiveFailureScenarios() {
    Line("-- §19 五场景故障测试（重复请求 / RPC 重试 / 断线重连 / Crash / DB 重试）\n");
    Env env;
    constexpr PlayerId kP = 30001;
    env.EnsureChar(kP);

    // 装配：幂等表 + 账本（持久层），模拟「GameNode + DataService」的真实关系
    InMemoryIdemTable idem_table;
    IdempotencyStore idem(idem_table);
    InMemoryLedgerStore ledger_store;

    auto build = [&](Ledger* ledger) {
        auto* eco = new EconomySystem(env.inv);
        (void)eco->SetPriceTable(env.prices);
        if (ledger != nullptr) eco->SetLedger(ledger);
        eco->SetIdempotencyStore(&idem);
        return eco;
    };

    Ledger ledger(ledger_store, LedgerConfig{1024, 3});
    EconomySystem* eco = build(&ledger);

    // 启动资金（走同一幂等 + 账本通道）
    {
        auto seed = Cmd(EconomyOp::AddCurrency, kP, "seed-crash", 100'000);
        CHECK(eco->Execute(seed, env.ctx).HasValue());
    }
    env.Drain();
    CHECK(ledger.Flush().HasValue());
    const std::int64_t start_balance = eco->Balance(kP, kCurrencyGold).Value();
    CHECK_EQ(start_balance, static_cast<std::int64_t>(100'000));

    // ---- 场景 1：重复请求（同一 key 连发 10 次）----
    {
        const std::string key = "dup:req:1";
        int applied = 0;
        int dedup = 0;
        for (int i = 0; i < 10; ++i) {
            auto c = Cmd(EconomyOp::RemoveCurrency, kP, key, 500);
            auto r = eco->Execute(c, env.ctx);
            CHECK(r.HasValue());
            if (r.Value().applied && !r.Value().deduplicated) ++applied;
            if (r.Value().deduplicated) ++dedup;
        }
        CHECK_EQ(applied, 1);
        CHECK_EQ(dedup, 9);
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), start_balance - 500);
        LineFmt("   [1] 重复请求 10 次：余额变化 1 次，deduplicated=9\n");
    }

    // ---- 场景 2：RPC 重试（注入 UNAVAILABLE/TIMEOUT，客户端用同一 key 重试 3 次）----
    {
        const std::string key = "rpc:retry:1";
        const std::int64_t before = eco->Balance(kP, kCurrencyGold).Value();
        // 第一次尝试在幂等占位阶段就瞬时失败 → 无任何副作用
        idem_table.FailNextInserts(1);
        auto first = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 300), env.ctx);
        CHECK(!first.HasValue());
        CHECK_EQ(static_cast<int>(first.Err().Code()), static_cast<int>(core::ErrorCode::TIMEOUT));
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), before);
        // 客户端重试 3 次，全部沿用同一个 key
        int ok_count = 0;
        int dedup_count = 0;
        for (int i = 0; i < 3; ++i) {
            auto r = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 300), env.ctx);
            CHECK(r.HasValue());
            if (!r.Value().deduplicated) ++ok_count;
            else ++dedup_count;
        }
        CHECK_EQ(ok_count, 1);
        CHECK_EQ(dedup_count, 2);
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), before - 300);  // 只扣 1 次
        LineFmt("   [2] RPC 重试 3 次（同 key）：只扣 1 次（%lld → %lld）\n",
                static_cast<long long>(before),
                static_cast<long long>(eco->Balance(kP, kCurrencyGold).Value()));
    }

    // ---- 场景 3：断线重连（扣钱成功 → 客户端立刻断线 → 重连后重发同 key）----
    //
    // 关键区分：**断线重连不是进程重启**——§6 规定实时状态（含余额）的 Owner 是承载该玩家的
    // GameNode，同一个节点不会因为客户端掉线就丢掉钱包。真正必须跨进程生效的是「幂等判定」，
    // 所以本场景分两步：
    //   3a 重连回同一节点：重发同 key → 返回首次结果，余额不再变化；
    //   3b 重连后被路由到**另一个 GameNode**（新建 EconomySystem，只共享持久层）：
    //      同 key 依然去重成功 —— 证明「不重复扣」不依赖进程内的幂等 map。
    //      注意此处**不能**断言新实例的钱包余额：钱包不跨进程，余额由 DataService 的持久化
    //      余额装载回来（TASK-026/027/028 的 Load），断言对象只能是「首次结果 + 账本行数」。
    {
        const std::string key = "reconnect:1";
        auto pay = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 700), env.ctx);
        CHECK(pay.HasValue() && pay.Value().applied);
        env.Drain();
        CHECK(ledger.Flush().HasValue());
        const std::int64_t after_pay = eco->Balance(kP, kCurrencyGold).Value();
        const std::size_t rows_after_pay = ledger_store.Size();
        const std::uint32_t first_version = pay.Value().version;

        // 3a：同一节点重连重发
        auto resend = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 700), env.ctx);
        CHECK(resend.HasValue());
        CHECK(resend.Value().deduplicated);
        CHECK_EQ(resend.Value().balance_after, after_pay);
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), after_pay);  // 不重复扣

        // 3b：换一个 GameNode 重发同一 key
        {
            auto* other = build(&ledger);
            auto resend2 = other->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 700), env.ctx);
            CHECK(resend2.HasValue());
            CHECK(resend2.Value().deduplicated);
            CHECK_EQ(resend2.Value().balance_after, after_pay);    // 首次结果原样返回
            CHECK_EQ(resend2.Value().version, first_version);
            CHECK_EQ(ledger_store.Size(), rows_after_pay);         // 账本没有第二条
            delete other;
        }
        LineFmt("   [3] 断线重连（同节点 + 换节点）：重发同 key 不重复扣（保持 %lld，账本 %zu 行不变）\n",
                static_cast<long long>(after_pay), rows_after_pay);
    }

    // ---- 场景 4：GameNode Crash（幂等行停在 InFlight + 副作用已落账 → 重启后不重复执行）----
    //
    // 复现窗口：`TryBegin` 成功 → 钱包扣减与账本落账都已完成 → 进程在 `Commit` 之前被强杀。
    // 「重启」= 丢弃全部进程内状态（钱包 + 幂等缓存），只保留持久层（幂等表 + 账本）。
    // 因此这里**直接驱动原语**构造崩溃现场，而不是再 `new` 一个 EconomySystem 去执行扣款：
    // 后者带的是一个**空钱包**，扣 900 会先被「余额不足」挡下，测到的是余额不足而不是崩溃恢复
    // （这正是本场景最初写错的地方）。
    //
    // 口径说明（为什么余额断言放在「重启后重新装载」之后）：§6 规定实时状态（含余额）的 Owner
    // 是承载该玩家的 GameNode，进程内 `WalletTable` 不跨进程存活；崩溃后的余额由 DataService
    // 的持久化余额装载回来（TASK-026/027/028 的 Load 路径，不在本任务范围）。所以本场景要证明
    // 的契约是：**同一幂等键绝不重复执行**——余额不被扣第二次、账本不新增第二行。
    {
        const std::string key = "crash:1";
        const std::int64_t before = eco->Balance(kP, kCurrencyGold).Value();
        const std::int64_t t0 = idem.NowMs();

        // (1) 崩溃前现场：幂等行已占位（InFlight，未 Commit），副作用已扣款并落账
        CHECK(idem.TryBegin(key, DurationMs{30'000}).Value() == IdemStatus::Fresh);
        {
            LEntry e;
            e.transaction_id = 900'001;
            e.request_id = 900'001;
            e.idempotency_key = key;
            e.player = kP;
            e.op = EconomyOp::RemoveCurrency;
            e.currency = kCurrencyGold;
            e.delta = -900;
            e.balance_after = before - 900;   // 崩溃前那次扣减后的余额快照
            e.reason = "crash-window";
            e.source = "test:ledger";
            e.timestamp_ms = 1'700'000'000'000LL;
            e.version = 1;
            CHECK(ledger.Append(e).HasValue());
            CHECK(ledger.Flush().HasValue());
        }

        // (2) 重启：丢弃全部进程内状态，只保留持久层
        delete eco;
        eco = build(&ledger);

        // (2a) 由持久层装载余额（模拟 DataService 的 Load：崩溃前那次扣款已持久化为 before-900）
        {
            auto reload = Cmd(EconomyOp::AddCurrency, kP, "crash:reload:1", before - 900);
            auto rr = eco->Execute(reload, env.ctx);
            CHECK(rr.HasValue() && rr.Value().applied);
            env.Drain();
            CHECK(ledger.Flush().HasValue());
        }
        const std::int64_t after_crash = before - 900;
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), after_crash);
        const std::size_t rows_after_crash = ledger_store.Size();

        // (3) TTL 未过期：InFlight 拦截 → BUSY；不执行、不扣款、不新增账本行
        {
            auto again = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 900), env.ctx);
            CHECK(again.HasValue());
            CHECK(!again.Value().applied);
            CHECK_EQ(static_cast<int>(again.Value().code), static_cast<int>(core::ErrorCode::BUSY));
            CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), after_crash);
            CHECK_EQ(ledger_store.Size(), rows_after_crash);
        }
        CHECK(eco->BusyCount() >= static_cast<std::uint64_t>(1));

        // (4) TTL 过期（§15.8）：查账本命中同 key 的已落账记录 → 重放首次结果，**不重复执行**
        idem.SetInjectedNowMs(t0 + 31'000);
        auto replay = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 900), env.ctx);
        CHECK(replay.HasValue());
        CHECK(replay.Value().deduplicated);
        CHECK(replay.Value().applied);
        CHECK_EQ(replay.Value().balance_after, after_crash);             // 首次结果原样返回
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), after_crash);  // 关键：没扣第二次
        CHECK_EQ(ledger_store.Size(), rows_after_crash);                 // 也没写第二条账
        idem.ClearInjectedNowMs();

        // (5) 恢复路径已把幂等行补记为 Completed → 再来一次直接命中首次结果
        auto again2 = eco->Execute(Cmd(EconomyOp::RemoveCurrency, kP, key, 900), env.ctx);
        CHECK(again2.HasValue());
        CHECK(again2.Value().deduplicated);
        CHECK_EQ(eco->Balance(kP, kCurrencyGold).Value(), after_crash);
        LineFmt("   [4] Crash + 重启 + TTL 过期：账本重放首次结果，余额保持 %lld（账本 %zu 行不变）\n",
                static_cast<long long>(after_crash), rows_after_crash);
    }

    // ---- 场景 5：数据库重试（落库注入死锁/超时 → 不产生两条账本记录）----
    {
        const std::string key = "dbfail:1";
        const std::size_t rows_before = ledger_store.Size();
        idem_table.FailNextInserts(0);

        // 5a：注入 2 次瞬时失败，Ledger 内部重试（max_retries=3）后成功 → 恰好 1 行
        {
            auto c = Cmd(EconomyOp::Reward, kP, key, 1200);
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
            auto r = eco->Execute(c, env.ctx);
            CHECK(r.HasValue() && r.Value().applied);
            env.Drain();
            ledger_store.FailNextAppends(2);
            CHECK(ledger.Flush().HasValue());
            CHECK_EQ(ledger_store.Size(), rows_before + 1);
            // 注入的 2 次瞬时失败被 WriteOne 内部重试吃掉，最终恰好写入 1 行
            CHECK_EQ(ledger_store.InjectedFailures(), static_cast<std::uint64_t>(2));
            CHECK(ledger.Stats().retries >= static_cast<std::uint64_t>(2));
        }

        // 5b：注入超过重试上限 → Flush 失败但**保留**待落库条目；故障消失后重投
        //     重投会重放已写入的条目 → 命中 UNIQUE → 不产生第二条
        {
            const std::size_t pending_before = ledger.PendingCount();
            for (int i = 0; i < 3; ++i) {
                auto c = Cmd(EconomyOp::Reward, kP, "dbfail:batch:" + std::to_string(i), 10);
                c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
                CHECK(eco->Execute(c, env.ctx).HasValue());
            }
            env.Drain();
            CHECK_EQ(ledger.PendingCount(), pending_before + 3);

            ledger_store.FailNextAppends(1000);
            auto failed = ledger.Flush();
            CHECK(!failed.HasValue());
            CHECK_EQ(ledger.PendingCount(), pending_before + 3);  // 一条都没丢

            ledger_store.FailNextAppends(0);
            CHECK(ledger.Flush().HasValue());
            CHECK_EQ(ledger_store.Size(), rows_before + 4);
            CHECK_EQ(ledger.PendingCount(), pending_before);  // 全批落库成功后排空
            CHECK(ledger.VerifyChain(0, 9'999'999'999'999LL).Value());
        }

        // 5c：绕过应用层，直接向存储写同 key → UNIQUE 必须拦住（数据库层兜底）
        {
            const std::size_t n = ledger_store.Size();
            LEntry dup;
            dup.transaction_id = 999'999;
            dup.idempotency_key = "dbfail:1";
            dup.player = kP;
            dup.timestamp_ms = 1;
            auto r = ledger_store.Append(dup);
            CHECK(r.HasValue());
            CHECK(!r.Value().stored);
            CHECK(r.Value().duplicate);
            CHECK_EQ(ledger_store.Size(), n);  // 没有第二条
            CHECK(ledger_store.DuplicateRejections() >= 1u);
        }
        LineFmt("   [5] 数据库重试：重放命中 UNIQUE，账本记录未翻倍（rows=%zu，重复拦截=%llu）\n",
                ledger_store.Size(),
                static_cast<unsigned long long>(ledger_store.DuplicateRejections()));
    }

    // ---- 额外：装备发放重复（同一幂等键不产出两个 ItemGuid）----
    {
        const std::string key = "item:grant:1";
        const PlayerId pi = 30002;
        env.EnsureChar(pi);
        const std::uint32_t before = env.CountItems(pi, 1);

        std::size_t guids_first = 0;
        for (int i = 0; i < 5; ++i) {
            auto c = Cmd(EconomyOp::AddItem, pi, key, 0);
            c.item_deltas.push_back(ItemDelta{1, 3, kInvalidItemGuid});
            auto r = eco->Execute(c, env.ctx);
            CHECK(r.HasValue());
            if (i == 0) {
                CHECK(r.Value().applied);
                guids_first = r.Value().created_guids.size();
            } else {
                CHECK(r.Value().deduplicated);
                CHECK_EQ(r.Value().created_guids.size(), guids_first);  // 不重复生成
            }
        }
        CHECK_EQ(env.CountItems(pi, 1), before + 3);  // 物品只发了一次
        LineFmt("   [额外] 装备发放重复 5 次：物品只 +3（不复制 ItemGuid）\n");
    }

    // 五场景结束时：幂等表无悬挂 InFlight（场景 4 的那条已在恢复路径被 Commit）
    CHECK_EQ(idem.InFlightCount(), static_cast<std::size_t>(0));
    env.Drain();
    CHECK(ledger.Flush().HasValue());
    CHECK(ledger.VerifyChain(0, 9'999'999'999'999LL).Value());
    delete eco;
}

// ------------------------------------------------------------------ §17 集成（1 万次）

void TestIntegration10000(DumpPaths& paths) {
    Line("-- §17 集成：1 万次混合经济操作（守恒 + 链 + 对账）\n");
    Env env;
    constexpr int kPlayers = 40;
    std::vector<PlayerId> ps;
    ps.reserve(kPlayers);
    for (int i = 0; i < kPlayers; ++i) {
        const PlayerId p = static_cast<PlayerId>(40000 + i);
        env.EnsureChar(p);
        ps.push_back(p);
    }

    InMemoryIdemTable idem_table;
    IdempotencyStore idem(idem_table);
    InMemoryLedgerStore ledger_store;
    Ledger ledger(ledger_store, LedgerConfig{4096, 3});

    EconomySystem eco(env.inv);
    CHECK(eco.SetPriceTable(env.prices).HasValue());
    eco.SetIdempotencyStore(&idem);
    eco.SetLedger(&ledger);

    // 初始发行：每个玩家一笔（走同一通道，账本从零摘要起链）
    for (int i = 0; i < kPlayers; ++i) {
        auto c = Cmd(EconomyOp::AddCurrency, ps[static_cast<std::size_t>(i)],
                     "init:" + std::to_string(i), 1'000'000);
        CHECK(eco.Execute(c, env.ctx).HasValue());
    }
    env.Drain();
    CHECK(ledger.Flush().HasValue());
    const std::int64_t issued = eco.Stats().total_minted;
    CHECK_EQ(issued, static_cast<std::int64_t>(kPlayers) * 1'000'000);

    // 1 万次混合操作（八种操作 + 物品，全部走 gold 单一币种以便守恒口径清晰）
    std::uint64_t seed = 20260910;
    auto rnd = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(seed >> 33);
    };
    std::size_t applied = 0;
    std::size_t rejected = 0;
    constexpr std::size_t kOps = 10000;
    for (std::size_t i = 0; i < kOps; ++i) {
        const std::uint32_t pick = rnd() % 100;
        const PlayerId p = ps[rnd() % static_cast<std::uint32_t>(kPlayers)];
        const std::string key = "mix:" + std::to_string(i);
        EconomyCommand c = Cmd(EconomyOp::AddCurrency, p, key);
        if (pick < 20) {
            c.op = EconomyOp::AddCurrency;
            c.amount = 1 + (rnd() % 500);
        } else if (pick < 40) {
            c.op = EconomyOp::RemoveCurrency;
            c.amount = 1 + (rnd() % 400);
        } else if (pick < 55) {
            c.op = EconomyOp::Transfer;
            // TASK-029 ValidateCommand：禁止 Transfer 给自己（peer == player 会被拒绝）
            do {
                c.peer = ps[rnd() % static_cast<std::uint32_t>(kPlayers)];
            } while (c.peer == p);
            c.amount = 1 + (rnd() % 300);
        } else if (pick < 70) {
            c.op = EconomyOp::Purchase;
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else if (pick < 85) {
            c.op = EconomyOp::Reward;
            c.amount = 1 + (rnd() % 200);
            // §TASK-029 ValidateCommand：Reward 属于「物品类操作」，必须至少带一条 item_delta
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else if (pick < 92) {
            c.op = EconomyOp::AddItem;
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else {
            c.op = EconomyOp::RemoveItem;
            c.item_deltas.push_back(ItemDelta{1, -1, kInvalidItemGuid});
        }
        auto r = eco.Execute(c, env.ctx);
        CHECK(r.HasValue());  // 命令合法 → 不允许出现「格式错误」
        if (r.HasValue() && r.Value().applied) ++applied;
        else ++rejected;
    }
    env.Drain();

    // 待落库必须能被排空，且幂等表不得有悬挂 InFlight（§17）
    CHECK(ledger.Flush().HasValue());
    CHECK_EQ(ledger.PendingCount(), static_cast<std::size_t>(0));
    CHECK_EQ(idem.InFlightCount(), static_cast<std::size_t>(0));
    CHECK_EQ(idem_table.Stats().inflight, static_cast<std::size_t>(0));
    // 已完成的幂等行 == 初始发行 40 条 + 实际生效的操作数（业务拒绝走 Abort，不留行）
    CHECK_EQ(idem_table.Stats().completed,
             static_cast<std::size_t>(kPlayers) + applied);

    // 资金守恒（§17）：Σ余额 == 累计发行 − 累计销毁
    const std::int64_t total_balance = eco.TotalBalance();
    const std::int64_t minted = eco.Stats().total_minted;
    const std::int64_t burned = eco.Stats().total_burned;
    CHECK_EQ(total_balance, minted - burned);

    // Σ账本净额 == Σ余额变化（逐玩家，含 Transfer 收款方的净额口径）
    std::int64_t sum_net = 0;
    for (const PlayerId p : ps) {
        const std::int64_t net = ledger_store.NetDeltaByPlayer(p);
        const std::int64_t bal = eco.Balance(p, kCurrencyGold).Value();
        CHECK_EQ(net, bal);  // 每个玩家的账本净额必须能解释其当前余额
        sum_net += net;
    }
    CHECK_EQ(sum_net, total_balance);
    CHECK_EQ(sum_net, minted - burned);

    // 哈希链完整可校验（§17 / §20.3）
    auto chain = ledger.VerifyChain(0, 9'999'999'999'999LL);
    CHECK(chain.HasValue() && chain.Value());

    // 落盘 CSV 供 tools/audit/economy_audit.py 对账（§20.4「对账工具零差异」）
    {
        std::error_code ec;
        std::filesystem::create_directories("bench", ec);

        std::ofstream ledger_csv(paths.ledger_csv, std::ios::binary);
        CHECK(static_cast<bool>(ledger_csv));
        ledger_csv << "transaction_id,idempotency_key,player,peer,op,currency,delta,balance_after,"
                      "version,timestamp_ms\n";
        auto rows = ledger_store.LoadRange(0, 9'999'999'999'999LL);
        CHECK(rows.HasValue());
        for (const LEntry& e : rows.Value()) {
            ledger_csv << e.transaction_id << ',' << e.idempotency_key << ',' << e.player << ','
                       << e.peer << ',' << static_cast<unsigned>(e.op) << ',' << e.currency << ','
                       << e.delta << ',' << e.balance_after << ',' << e.version << ','
                       << e.timestamp_ms << '\n';
        }
        ledger_csv.close();

        std::ofstream bal_csv(paths.balances_csv, std::ios::binary);
        CHECK(static_cast<bool>(bal_csv));
        bal_csv << "player,currency,balance\n";
        for (const PlayerId p : ps) {
            bal_csv << p << ',' << kCurrencyGold << ','
                    << eco.Balance(p, kCurrencyGold).Value() << '\n';
        }
        bal_csv.close();

        std::ofstream meta(paths.meta_txt, std::ios::binary);
        CHECK(static_cast<bool>(meta));
        meta << "ops=" << kOps << "\n"
             << "players=" << kPlayers << "\n"
             << "applied=" << applied << "\n"
             << "rejected=" << rejected << "\n"
             << "ledger_rows=" << ledger_store.Size() << "\n"
             << "issued=" << minted << "\n"
             << "burned=" << burned << "\n"
             << "total_balance=" << total_balance << "\n";
        meta.close();
    }

    LineFmt("   ops=%zu applied=%zu rejected=%zu ledger_rows=%zu minted=%lld burned=%lld "
            "total_balance=%lld\n",
            kOps, applied, rejected, ledger_store.Size(), static_cast<long long>(minted),
            static_cast<long long>(burned), static_cast<long long>(total_balance));
    LineFmt("   链校验=%s；逐玩家 Σ账本净额 == 当前余额（%d 名玩家全部通过）\n",
            chain.Value() ? "PASS" : "FAIL", kPlayers);
}

}  // namespace

int main() {
    DumpPaths paths{"bench/economy_ledger_dump.csv", "bench/economy_balances.csv",
                          "bench/economy_audit_meta.txt"};
    TestSha256KnownVectors();
    TestLEntryHash();
    TestIdempotencyStateMachine();
    TestLedgerAppendFlushVerify();
    TestFiveFailureScenarios();
    TestIntegration10000(paths);

    LineFmt("ledger_test: checks=%d failures=%d\n", g_checks, g_fail);
    if (g_fail == 0) Line("ALL TESTS PASSED\n");
    return g_fail == 0 ? 0 : 1;
}
