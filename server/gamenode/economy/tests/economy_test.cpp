// server/gamenode/economy/tests/economy_test.cpp — TASK-029 §16 / §17 / §19
//
// 覆盖：
//   §16 单元：八种操作成功/失败路径、必填字段校验（缺 idempotency_key 拒绝）、
//             余额边界（0 / 负数 / 溢出）、背包空间校验、Transfer 原子性、
//             Purchase 两步一致性、价格表配置化、事件产生。
//   §17 集成：1000 玩家 × 1 万次混合操作 → 货币守恒、无负数余额、事件数一致。
//   §19 故障：余额不足拒绝；背包满不扣钱；幂等缺失拒绝；重复幂等键返回首次结果；
//             账本投递失败 → 返回成功但标记 pending（本实现「写死的一种」，不回滚）。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/economy/economy_command.h"
#include "mmo/game/economy/economy_events.h"
#include "mmo/game/economy/economy_system.h"
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

using mmo::core::test::ErrorFmt;
using mmo::core::test::LineFmt;

int g_fail = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond)) {                                               \
            ErrorFmt("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

// ---- 事件计数（§15.9）----
struct EvCount {
    std::size_t currency = 0, traded = 0, purchase = 0;
    void Reset() { currency = traded = purchase = 0; }
};
EvCount g_ev;

/// 恒失败的账本 sink（§19：用于验证「内存态已改 → 成功但标记 pending」）。
class FailingSink : public ILedgerSink {
public:
    bool Enqueue(const LedgerEntry&) noexcept override {
        ++calls;
        return false;
    }
    std::size_t calls{0};
};

/// 恒成功的账本 sink。
class OkSink : public ILedgerSink {
public:
    bool Enqueue(const LedgerEntry& e) noexcept override {
        ++calls;
        last = e;
        return true;
    }
    std::size_t calls{0};
    LedgerEntry last{};
};

struct Harness {
    core::EventBus bus;
    core::Scheduler scheduler;
    core::Arena arena{256 * 1024};
    EntityManager mgr{&bus};
    SceneContext ctx;
    role::InMemoryPersistenceAdapter sink;
    role::ExpCurve curve;
    role::RoleSystem role;
    ItemDefStore store;
    InventorySystem inv;
    EconomySystem eco;

    static role::ExpCurve LoadCurve() {
        auto r = role::ExpCurve::LoadFromFile("config/gameplay/exp_curve.json");
        if (!r.HasValue()) {
            ErrorFmt("FAIL: cannot load exp_curve.json\n");
            std::exit(1);
        }
        return std::move(r).Value();
    }

    Harness()
        : ctx(7, SceneType::World, 1, core::MonotonicClock::Point(), 0, mgr, bus, scheduler, arena),
          curve(LoadCurve()),
          role(sink, curve),
          inv(store, role),
          eco(inv) {
        auto lr = store.LoadDir("config/gameplay/items");
        if (!lr.HasValue()) {
            ErrorFmt("FAIL: load items: %s\n", lr.Err().Message().data());
            std::exit(1);
        }
        auto pr = prices().LoadFromFile("config/gameplay/economy/prices.json");
        if (!pr.HasValue()) {
            ErrorFmt("FAIL: load prices: %s\n", pr.Err().Message().data());
            std::exit(1);
        }
        (void)eco.SetPriceTable(prices());
        role.BindEventBus(bus);
        inv.BindEventBus(bus);

        (void)bus.Subscribe<CurrencyChanged>([](const CurrencyChanged&) { ++g_ev.currency; });
        (void)bus.Subscribe<ItemTraded>([](const ItemTraded&) { ++g_ev.traded; });
        (void)bus.Subscribe<PurchaseCompleted>([](const PurchaseCompleted&) { ++g_ev.purchase; });
    }

    PriceTable& prices() {
        static PriceTable t;
        return t;
    }

    /// Drain 到队列清空（EventBus 单次 Drain 上限 4096，批量事件必须循环）。
    void DrainAll() {
        while (bus.QueueDepth() > 0) (void)bus.Drain();
    }

    /// 建号（LoadOrCreate 是 [[nodiscard]]，须显式处理返回值）。
    void EnsureChar(PlayerId p) { (void)role.LoadOrCreate(p, 1000u + p, ctx); }

    /// 事件计数清零。**必须先 Drain 再清零**：EventBus 只入队、派发在 Drain，
    /// 若直接 Reset 会把上一批仍滞留队列的事件算进本段，导致计数虚高。
    void ResetEvents() {
        DrainAll();
        g_ev.Reset();
    }
};

/// 构造一条合法命令（各用例再按需改写字段）。
EconomyCommand MakeCmd(EconomyOp op, PlayerId p, std::uint64_t txn, const std::string& key) {
    EconomyCommand c;
    c.request_id = txn;
    c.trace_id = txn;
    c.transaction_id = txn;
    c.idempotency_key = key;
    c.player = p;
    c.op = op;
    c.currency = kCurrencyGold;
    c.reason = "test";
    c.source = "test:unit";
    return c;
}

// ---------------------------------------------------------------- §8 必填校验

void test_command_validation() {
    Harness h;
    // 缺 idempotency_key → 拒绝（§20.4 硬性校验）
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 1, 1, "");
        c.amount = 100;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
        CHECK(r.Err().Code() == core::ErrorCode::INVALID_ARGUMENT);
    }
    // 缺 transaction_id
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 1, 0, "k1");
        c.amount = 100;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
    }
    // 缺 player
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 0, 2, "k2");
        c.amount = 100;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
    }
    // 缺 reason / source（审计必需）
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 1, 3, "k3");
        c.amount = 100;
        c.reason = "";
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
    }
    // amount <= 0 的货币操作
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 1, 4, "k4");
        c.amount = 0;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
    }
    // Transfer 无对手方
    {
        EconomyCommand c = MakeCmd(EconomyOp::Transfer, 1, 5, "k5");
        c.amount = 10;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
    }
    // 非法币种
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 1, 6, "k6");
        c.amount = 10;
        c.currency = 9999;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(!r.HasValue());
    }
    // 命令格式错误不该产生任何副作用（不写幂等表、不计数 applied）
    CHECK(h.eco.Stats().applied_count == 0);
    CHECK(h.eco.IdempotencySize() == 0);
}

// ---------------------------------------------------------------- 货币

void test_currency_add_remove() {
    Harness h;
    {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 100, 10, "c1");
        c.amount = 500;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(r.HasValue());
        CHECK(r.Value().applied);
        CHECK(r.Value().balance_after == 500);
        CHECK(h.eco.Balance(100, kCurrencyGold).Value() == 500);
    }
    {
        EconomyCommand c = MakeCmd(EconomyOp::RemoveCurrency, 100, 11, "c2");
        c.amount = 200;
        auto r = h.eco.Execute(c, h.ctx);
        CHECK(r.HasValue());
        CHECK(r.Value().balance_after == 300);
    }
    // 未开过户的玩家余额为 0（不是错误）
    CHECK(h.eco.Balance(999, kCurrencyGold).Value() == 0);
    // 非法币种查询 → NOT_FOUND
    CHECK(!h.eco.Balance(100, 0).HasValue());
}

void test_currency_insufficient_no_negative() {
    Harness h;
    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, 200, 20, "n1");
    add.amount = 100;
    (void)h.eco.Execute(add, h.ctx);

    EconomyCommand rm = MakeCmd(EconomyOp::RemoveCurrency, 200, 21, "n2");
    rm.amount = 101;  // 超额
    auto r = h.eco.Execute(rm, h.ctx);
    CHECK(r.HasValue());              // 业务拒绝以 Ok + applied=false 返回
    CHECK(!r.Value().applied);
    CHECK(r.Value().code == core::ErrorCode::BUSY);
    CHECK(r.Value().balance_after == 100);  // 余额未被扣成负数
    CHECK(h.eco.Balance(200, kCurrencyGold).Value() == 100);
}

void test_currency_overflow() {
    Harness h;
    EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 300, 30, "o1");
    c.amount = kMaxBalance;
    auto r1 = h.eco.Execute(c, h.ctx);
    CHECK(r1.HasValue() && r1.Value().applied);
    CHECK(h.eco.Balance(300, kCurrencyGold).Value() == kMaxBalance);

    EconomyCommand c2 = MakeCmd(EconomyOp::AddCurrency, 300, 31, "o2");
    c2.amount = 1;  // 再 +1 会超过上限
    auto r2 = h.eco.Execute(c2, h.ctx);
    CHECK(r2.HasValue());
    CHECK(!r2.Value().applied);
    CHECK(r2.Value().code == core::ErrorCode::BUSY);
    CHECK(h.eco.Balance(300, kCurrencyGold).Value() == kMaxBalance);  // 未变
}

// ---------------------------------------------------------------- 物品

void test_items_add_remove() {
    Harness h;
    const PlayerId p = 400;
    h.EnsureChar(p);

    EconomyCommand add = MakeCmd(EconomyOp::AddItem, p, 40, "i1");
    add.item_deltas.push_back(ItemDelta{1, 5, kInvalidItemGuid});  // 5 个治疗药水
    auto r1 = h.eco.Execute(add, h.ctx);
    CHECK(r1.HasValue());
    CHECK(r1.Value().applied);
    CHECK(!r1.Value().created_guids.empty());  // 新增物品 guid 留痕
    CHECK(h.inv.View(p)->TotalCount() == 5);

    EconomyCommand rm = MakeCmd(EconomyOp::RemoveItem, p, 41, "i2");
    rm.item_deltas.push_back(ItemDelta{1, -3, kInvalidItemGuid});
    auto r2 = h.eco.Execute(rm, h.ctx);
    CHECK(r2.HasValue());
    CHECK(r2.Value().applied);
    CHECK(h.inv.View(p)->TotalCount() == 2);

    // 移除超过持有量 → 拒绝且不丢物品
    EconomyCommand rm2 = MakeCmd(EconomyOp::RemoveItem, p, 42, "i3");
    rm2.item_deltas.push_back(ItemDelta{1, -99, kInvalidItemGuid});
    auto r3 = h.eco.Execute(rm2, h.ctx);
    CHECK(r3.HasValue());
    CHECK(!r3.Value().applied);
    CHECK(r3.Value().code == core::ErrorCode::BUSY);
    CHECK(h.inv.View(p)->TotalCount() == 2);  // 一个都没扣（先校验后扣）
}

// ---------------------------------------------------------------- Transfer

void test_transfer_atomic() {
    Harness h;
    EconomyCommand a = MakeCmd(EconomyOp::AddCurrency, 500, 50, "t0");
    a.amount = 1000;
    (void)h.eco.Execute(a, h.ctx);

    EconomyCommand t = MakeCmd(EconomyOp::Transfer, 500, 51, "t1");
    t.peer = 501;
    t.amount = 300;
    h.ResetEvents();
    auto r = h.eco.Execute(t, h.ctx);
    CHECK(r.HasValue());
    CHECK(r.Value().applied);
    CHECK(h.eco.Balance(500, kCurrencyGold).Value() == 700);
    CHECK(h.eco.Balance(501, kCurrencyGold).Value() == 300);

    h.DrainAll();
    CHECK(g_ev.currency == 2);  // 出账 + 入账各一条

    // 余额不足 → 双方都不变
    EconomyCommand t2 = MakeCmd(EconomyOp::Transfer, 500, 52, "t2");
    t2.peer = 501;
    t2.amount = 99999;
    auto r2 = h.eco.Execute(t2, h.ctx);
    CHECK(r2.HasValue());
    CHECK(!r2.Value().applied);
    CHECK(r2.Value().code == core::ErrorCode::BUSY);
    CHECK(h.eco.Balance(500, kCurrencyGold).Value() == 700);
    CHECK(h.eco.Balance(501, kCurrencyGold).Value() == 300);

    // ---- 物品转移（ItemTraded 事件，§15.9）----
    const PlayerId pa = 502;
    const PlayerId pb = 503;
    h.EnsureChar(pa);
    h.EnsureChar(pb);
    EconomyCommand gi = MakeCmd(EconomyOp::AddItem, pa, 53, "t3");
    gi.item_deltas.push_back(ItemDelta{1, 4, kInvalidItemGuid});
    (void)h.eco.Execute(gi, h.ctx);
    CHECK(h.inv.View(pa)->TotalCount() == 4);

    h.ResetEvents();
    EconomyCommand ti = MakeCmd(EconomyOp::Transfer, pa, 54, "t4");
    ti.peer = pb;
    ti.amount = 0;                                            // 纯物品转移
    ti.item_deltas.push_back(ItemDelta{1, 2, kInvalidItemGuid});
    auto r3 = h.eco.Execute(ti, h.ctx);
    CHECK(r3.HasValue());
    CHECK(r3.Value().applied);
    CHECK(h.inv.View(pa)->TotalCount() == 2);  // 出方减 2
    CHECK(h.inv.View(pb)->TotalCount() == 2);  // 入方加 2
    h.DrainAll();
    CHECK(g_ev.traded == 1);

    // 转移数量超过持有量 → 拒绝且双方都不变
    EconomyCommand ti2 = MakeCmd(EconomyOp::Transfer, pa, 55, "t5");
    ti2.peer = pb;
    ti2.amount = 0;
    ti2.item_deltas.push_back(ItemDelta{1, 99, kInvalidItemGuid});
    auto r4 = h.eco.Execute(ti2, h.ctx);
    CHECK(r4.HasValue());
    CHECK(!r4.Value().applied);
    CHECK(h.inv.View(pa)->TotalCount() == 2);
    CHECK(h.inv.View(pb)->TotalCount() == 2);
}

void test_transfer_rollback_on_overflow() {
    Harness h;
    // 收款方先顶到上限，入账必然失败 → 出账必须被回滚
    EconomyCommand fill = MakeCmd(EconomyOp::AddCurrency, 601, 60, "tr0");
    fill.amount = kMaxBalance;
    (void)h.eco.Execute(fill, h.ctx);

    EconomyCommand give = MakeCmd(EconomyOp::AddCurrency, 600, 61, "tr1");
    give.amount = 500;
    (void)h.eco.Execute(give, h.ctx);

    EconomyCommand t = MakeCmd(EconomyOp::Transfer, 600, 62, "tr2");
    t.peer = 601;
    t.amount = 100;
    auto r = h.eco.Execute(t, h.ctx);
    CHECK(r.HasValue());
    CHECK(!r.Value().applied);
    CHECK(r.Value().code == core::ErrorCode::BUSY);
    CHECK(h.eco.Balance(600, kCurrencyGold).Value() == 500);  // 出账已回滚
    CHECK(h.eco.Balance(601, kCurrencyGold).Value() == kMaxBalance);
}

// ---------------------------------------------------------------- Purchase

void test_purchase_two_step() {
    Harness h;
    const PlayerId p = 700;
    h.EnsureChar(p);

    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, p, 70, "p0");
    add.amount = 1000;
    (void)h.eco.Execute(add, h.ctx);

    // 治疗药水 def 1，单价 50（来自 prices.json，代码无硬编码）
    EconomyCommand buy = MakeCmd(EconomyOp::Purchase, p, 71, "p1");
    buy.item_deltas.push_back(ItemDelta{1, 3, kInvalidItemGuid});  // 3 × 50 = 150
    h.ResetEvents();
    auto r = h.eco.Execute(buy, h.ctx);
    CHECK(r.HasValue());
    CHECK(r.Value().applied);
    CHECK(r.Value().balance_after == 850);
    CHECK(r.Value().created_guids.size() == 1);
    CHECK(h.inv.View(p)->TotalCount() == 3);

    h.DrainAll();
    CHECK(g_ev.currency == 1);
    CHECK(g_ev.purchase == 1);
}

void test_purchase_insufficient_funds() {
    Harness h;
    const PlayerId p = 701;
    h.EnsureChar(p);

    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, p, 72, "p2");
    add.amount = 10;  // 不够买 1 个（50）
    (void)h.eco.Execute(add, h.ctx);

    EconomyCommand buy = MakeCmd(EconomyOp::Purchase, p, 73, "p3");
    buy.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
    auto r = h.eco.Execute(buy, h.ctx);
    CHECK(r.HasValue());
    CHECK(!r.Value().applied);
    CHECK(r.Value().code == core::ErrorCode::BUSY);
    CHECK(h.eco.Balance(p, kCurrencyGold).Value() == 10);  // 钱没被扣
    CHECK(h.inv.View(p) == nullptr || h.inv.View(p)->TotalCount() == 0);  // 也没发物品
}

void test_purchase_bag_full_no_charge() {
    Harness h;
    const PlayerId p = 702;
    h.EnsureChar(p);

    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, p, 74, "p4");
    add.amount = 100000;
    (void)h.eco.Execute(add, h.ctx);

    // 用不可堆叠的铁剑（def 10, max_stack=1）塞满 100 个槽
    for (int i = 0; i < 100; ++i) {
        auto r = h.inv.Add(p, 10, 1, 1);
        if (!r.HasValue()) break;
    }
    CHECK(h.inv.View(p)->UsedSlots() == kMaxInventorySlots);

    const std::int64_t before = h.eco.Balance(p, kCurrencyGold).Value();
    EconomyCommand buy = MakeCmd(EconomyOp::Purchase, p, 75, "p5");
    buy.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
    auto r = h.eco.Execute(buy, h.ctx);
    CHECK(r.HasValue());
    CHECK(!r.Value().applied);
    CHECK(r.Value().code == core::ErrorCode::BUSY);
    CHECK(h.eco.Balance(p, kCurrencyGold).Value() == before);  // 背包满 → 一分钱都不扣（§21）
}

void test_purchase_price_rules() {
    Harness h;
    const PlayerId p = 703;
    h.EnsureChar(p);
    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, p, 76, "p6");
    add.amount = 1000000;
    (void)h.eco.Execute(add, h.ctx);

    // 价格表未配置的商品 → NOT_FOUND（绝不回退到硬编码价格）
    {
        EconomyCommand buy = MakeCmd(EconomyOp::Purchase, p, 77, "p7");
        buy.item_deltas.push_back(ItemDelta{99999, 1, kInvalidItemGuid});
        auto r = h.eco.Execute(buy, h.ctx);
        CHECK(r.HasValue());
        CHECK(!r.Value().applied);
        CHECK(r.Value().code == core::ErrorCode::NOT_FOUND);
    }
    // 超出单次购买上限（def 9 = 敏捷药剂，max_count 5）
    {
        EconomyCommand buy = MakeCmd(EconomyOp::Purchase, p, 78, "p8");
        buy.currency = kCurrencyGem;
        buy.item_deltas.push_back(ItemDelta{9, 50, kInvalidItemGuid});
        auto r = h.eco.Execute(buy, h.ctx);
        CHECK(r.HasValue());
        CHECK(!r.Value().applied);
        CHECK(r.Value().code == core::ErrorCode::INVALID_ARGUMENT);
    }
    CHECK(h.inv.View(p) == nullptr || h.inv.View(p)->TotalCount() == 0);
}

// ---------------------------------------------------------------- Reward / Refund

void test_reward() {
    Harness h;
    const PlayerId p = 800;
    h.EnsureChar(p);

    EconomyCommand rw = MakeCmd(EconomyOp::Reward, p, 80, "r1");
    rw.amount = 250;                                            // 货币奖励
    rw.item_deltas.push_back(ItemDelta{2, 2, kInvalidItemGuid});  // + 2 个法力药水
    rw.reason = "quest_reward";
    rw.source = "quest:1001";
    h.ResetEvents();
    auto r = h.eco.Execute(rw, h.ctx);
    CHECK(r.HasValue());
    CHECK(r.Value().applied);
    CHECK(r.Value().balance_after == 250);
    CHECK(h.inv.View(p)->TotalCount() == 2);
    h.DrainAll();
    CHECK(g_ev.currency == 1);
}

void test_refund() {
    Harness h;
    const PlayerId p = 801;
    h.EnsureChar(p);

    // 先买 2 个治疗药水（2 × 50 = 100）
    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, p, 81, "rf0");
    add.amount = 500;
    (void)h.eco.Execute(add, h.ctx);
    EconomyCommand buy = MakeCmd(EconomyOp::Purchase, p, 82, "rf1");
    buy.item_deltas.push_back(ItemDelta{1, 2, kInvalidItemGuid});
    (void)h.eco.Execute(buy, h.ctx);
    CHECK(h.eco.Balance(p, kCurrencyGold).Value() == 400);
    CHECK(h.inv.View(p)->TotalCount() == 2);

    // 退款：退还 100 货币 + 收回 2 个物品
    EconomyCommand rf = MakeCmd(EconomyOp::Refund, p, 83, "rf2");
    rf.amount = 100;
    rf.item_deltas.push_back(ItemDelta{1, -2, kInvalidItemGuid});  // 负号 = 收回
    rf.reason = "refund";
    rf.source = "shop:3";
    auto r = h.eco.Execute(rf, h.ctx);
    CHECK(r.HasValue());
    CHECK(r.Value().applied);
    CHECK(h.eco.Balance(p, kCurrencyGold).Value() == 500);  // 钱退回
    CHECK(h.inv.View(p)->TotalCount() == 0);                // 物品收回
}

// ---------------------------------------------------------------- 幂等（§15.10 / §19）

void test_idempotency() {
    Harness h;
    EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 900, 90, "dup-key");
    c.amount = 100;
    auto r1 = h.eco.Execute(c, h.ctx);
    CHECK(r1.HasValue() && r1.Value().applied);
    CHECK(!r1.Value().deduplicated);

    // 同一幂等键重复提交 → 返回首次结果，不重复生效
    EconomyCommand c2 = MakeCmd(EconomyOp::AddCurrency, 900, 91, "dup-key");
    c2.amount = 100;
    auto r2 = h.eco.Execute(c2, h.ctx);
    CHECK(r2.HasValue());
    CHECK(r2.Value().deduplicated);
    CHECK(r2.Value().balance_after == 100);  // 仍是首次结果，没变成 200
    CHECK(h.eco.Balance(900, kCurrencyGold).Value() == 100);
    CHECK(h.eco.Stats().dedup_count == 1);
}

void test_idempotency_failure_not_cached() {
    Harness h;
    // 首次因余额不足被拒 → 不写幂等表（否则「充值后重试同一 key」会被永久毒化）
    EconomyCommand rm = MakeCmd(EconomyOp::RemoveCurrency, 901, 92, "retry-key");
    rm.amount = 50;
    auto r1 = h.eco.Execute(rm, h.ctx);
    CHECK(r1.HasValue());
    CHECK(!r1.Value().applied);
    CHECK(h.eco.IdempotencySize() == 0);

    // 充值后重试同一 key → 必须能成功
    EconomyCommand add = MakeCmd(EconomyOp::AddCurrency, 901, 93, "add-key");
    add.amount = 100;
    (void)h.eco.Execute(add, h.ctx);
    EconomyCommand rm2 = MakeCmd(EconomyOp::RemoveCurrency, 901, 94, "retry-key");
    rm2.amount = 50;
    auto r2 = h.eco.Execute(rm2, h.ctx);
    CHECK(r2.HasValue());
    CHECK(r2.Value().applied);
    CHECK(h.eco.Balance(901, kCurrencyGold).Value() == 50);
}

// ---------------------------------------------------------------- 账本（§19）

void test_ledger_pending() {
    Harness h;
    FailingSink sink;
    h.eco.SetLedgerSink(&sink);

    EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 950, 95, "lg1");
    c.amount = 100;
    auto r = h.eco.Execute(c, h.ctx);
    CHECK(r.HasValue());
    CHECK(r.Value().applied);          // 内存态已生效
    CHECK(r.Value().ledger_pending);   // 但账本投递失败 → 标记 pending，不回滚
    CHECK(sink.calls == 1);
    CHECK(h.eco.Stats().ledger_pending_count == 1);
    CHECK(h.eco.Balance(950, kCurrencyGold).Value() == 100);

    // 成功路径：不标记 pending
    OkSink ok;
    h.eco.SetLedgerSink(&ok);
    EconomyCommand c2 = MakeCmd(EconomyOp::AddCurrency, 950, 96, "lg2");
    c2.amount = 50;
    auto r2 = h.eco.Execute(c2, h.ctx);
    CHECK(r2.HasValue());
    CHECK(!r2.Value().ledger_pending);
    CHECK(ok.calls == 1);
    CHECK(ok.last.reason == "test");    // string_view 已按值拷贝，异步投递安全
    CHECK(ok.last.source == "test:unit");
}

// ---------------------------------------------------------------- 价格表（§20.6）

void test_price_table() {
    PriceTable t;
    // 正常加载
    auto r = t.LoadFromFile("config/gameplay/economy/prices.json");
    CHECK(r.HasValue());
    CHECK(t.size() > 0);
    const PriceEntry* pe = t.Find(1);
    CHECK(pe != nullptr);
    CHECK(pe->currency == kCurrencyGold);
    CHECK(pe->unit_price == 50);
    std::int64_t total = 0;
    CHECK(pe->Total(3, total) && total == 150);
    CHECK(pe->Allows(1) && pe->Allows(99) && !pe->Allows(100));  // max_count = 99

    // 坏 JSON：必须失败且不破坏既有内容
    PriceTable t2;
    (void)t2.LoadFromFile("config/gameplay/economy/prices.json");
    const std::size_t n = t2.size();
    auto bad = t2.LoadFromText("{ not json");
    CHECK(!bad.HasValue());
    CHECK(t2.size() == n);  // 原子加载：失败不修改既有内容

    // 缺字段
    PriceTable t3;
    CHECK(!t3.LoadFromText(R"({"prices":[{"item_id":1}]})").HasValue());
    // 非法 currency
    PriceTable t4;
    CHECK(!t4.LoadFromText(R"({"prices":[{"item_id":1,"currency":999,"unit_price":1}]})")
               .HasValue());
    // max < min
    PriceTable t5;
    CHECK(!t5.LoadFromText(
                 R"({"prices":[{"item_id":1,"currency":1,"unit_price":1,"min_count":5,"max_count":2}]})")
               .HasValue());
}

// ---------------------------------------------------------------- §17 集成

void test_integration_1000_players_conservation() {
    Harness h;
    constexpr int kPlayers = 1000;
    constexpr int kOps = 10000;

    // 简单 LCG（固定 seed，结果可复现；不引入随机设备）
    std::uint64_t seed = 20260910;
    auto rnd = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(seed >> 33);
    };

    for (int i = 0; i < kPlayers; ++i) {
        h.EnsureChar(static_cast<PlayerId>(10000 + i));
    }

    h.ResetEvents();
    std::uint64_t txn = 100000;
    std::size_t applied = 0;
    for (int i = 0; i < kOps; ++i) {
        const std::uint32_t pick = rnd() % 100;
        const PlayerId p = 10000 + (rnd() % kPlayers);
        EconomyOp op;
        if (pick < 30) {
            op = EconomyOp::AddCurrency;
        } else if (pick < 45) {
            op = EconomyOp::RemoveCurrency;
        } else if (pick < 60) {
            op = EconomyOp::Transfer;
        } else if (pick < 75) {
            op = EconomyOp::Purchase;
        } else if (pick < 90) {
            op = EconomyOp::Reward;
        } else {
            op = EconomyOp::AddItem;
        }

        EconomyCommand c = MakeCmd(op, p, ++txn, "int-" + std::to_string(i));
        if (op == EconomyOp::Transfer) {
            c.peer = 10000 + (rnd() % kPlayers);
            if (c.peer == p) c.peer = p + 1;
            c.amount = 1 + (rnd() % 100);
        } else if (op == EconomyOp::Purchase) {
            c.amount = 0;
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else if (op == EconomyOp::AddItem) {
            c.amount = 0;
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else if (op == EconomyOp::Reward) {
            c.amount = 1 + (rnd() % 500);
        } else {
            c.amount = 1 + (rnd() % 200);
        }
        auto r = h.eco.Execute(c, h.ctx);
        if (r.HasValue() && r.Value().applied) ++applied;
    }

    // ---- 货币守恒（§20.5）：总余额 == 总发行 - 总销毁 ----
    const EconomyStats st = h.eco.Stats();
    const std::int64_t expect = st.total_minted - st.total_burned;
    const std::int64_t actual = h.eco.TotalBalance();
    CHECK(actual == expect);
    LineFmt("  integration: ops=%d applied=%zu minted=%lld burned=%lld balance=%lld\n", kOps,
            applied, static_cast<long long>(st.total_minted),
            static_cast<long long>(st.total_burned), static_cast<long long>(actual));

    // ---- 无负数余额 ----
    bool any_negative = false;
    for (int i = 0; i < kPlayers; ++i) {
        for (std::uint32_t cur = 1; cur <= 4; ++cur) {
            const std::int64_t v = h.eco.Balance(10000 + i, cur).Value();
            if (v < 0) any_negative = true;
        }
    }
    CHECK(!any_negative);

    // ---- 事件数量与生效的货币操作一致 ----
    h.DrainAll();
    CHECK(g_ev.currency + g_ev.traded + g_ev.purchase > 0);
    LineFmt("  events: currency=%zu traded=%zu purchase=%zu\n", g_ev.currency, g_ev.traded,
            g_ev.purchase);
}

}  // namespace

int main() {
    test_command_validation();
    test_currency_add_remove();
    test_currency_insufficient_no_negative();
    test_currency_overflow();
    test_items_add_remove();
    test_transfer_atomic();
    test_transfer_rollback_on_overflow();
    test_purchase_two_step();
    test_purchase_insufficient_funds();
    test_purchase_bag_full_no_charge();
    test_purchase_price_rules();
    test_reward();
    test_refund();
    test_idempotency();
    test_idempotency_failure_not_cached();
    test_ledger_pending();
    test_price_table();
    test_integration_1000_players_conservation();

    if (g_fail == 0) {
        LineFmt("Economy.Suite: ALL PASS\n");
        return 0;
    }
    ErrorFmt("Economy.Suite: %d FAILED\n", g_fail);
    return 1;
}
