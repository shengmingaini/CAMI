// server/gamenode/economy/benchmark/economy_bench.cpp — TASK-029 §18 / §22
//
// 输出机器可读 key=value 到 bench/economy.txt（供 assert_metric 解析）：
//   execute_ns        单次经济命令（混合八种操作）平均耗时          阈值 ≤ 2000ns
//   balance_query_ns  单次余额查询平均耗时                          阈值 ≤   50ns
//   transfer_ns       单次 Transfer（双人原子）平均耗时              （观测）
//   purchase_ns       单次 Purchase（查价+扣币+发物品）平均耗时      （观测）
//   dedup_check_ns    单次幂等表命中查询平均耗时                     §22 ≤ 200ns
//
// 计时口径：所有指标都是**批量摊销**（外层取一次时钟），避免把 QPC 调用成本
// （≈17ns 硬地板）计入单次耗时 —— 否则 balance_query_ns 的 50ns 预算会被测量
// 本身吃掉三成以上，测出来的是时钟而不是代码。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_print.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/economy/currency.h"
#include "mmo/game/economy/economy_command.h"
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
using mmo::core::test::LineFmt;

std::uint64_t ElapsedNs(std::chrono::steady_clock::time_point t0) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
}

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
    PriceTable prices;

    static role::ExpCurve LoadCurve() {
        auto r = role::ExpCurve::LoadFromFile("config/gameplay/exp_curve.json");
        if (!r.HasValue()) {
            LineFmt("FAIL: cannot load exp_curve.json\n");
            std::exit(1);
        }
        return std::move(r).Value();
    }

    /// 建号（LoadOrCreate 是 [[nodiscard]]，须显式处理返回值）。
    void EnsureChar(PlayerId p) { (void)role.LoadOrCreate(p, 1000u + p, ctx); }

    Harness()
        : ctx(7, SceneType::World, 1, core::MonotonicClock::Point(), 0, mgr, bus, scheduler, arena),
          curve(LoadCurve()),
          role(sink, curve),
          inv(store, role),
          eco(inv) {
        (void)store.LoadDir("config/gameplay/items");
        (void)prices.LoadFromFile("config/gameplay/economy/prices.json");
        (void)eco.SetPriceTable(prices);
        role.BindEventBus(bus);
        inv.BindEventBus(bus);
    }
};

EconomyCommand MakeCmd(EconomyOp op, PlayerId p, std::uint64_t txn, const std::string& key) {
    EconomyCommand c;
    c.request_id = txn;
    c.trace_id = txn;
    c.transaction_id = txn;
    c.idempotency_key = key;
    c.player = p;
    c.op = op;
    c.currency = kCurrencyGold;
    c.reason = "bench";
    c.source = "bench:economy";
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 10000;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--ops" && i + 1 < argc) {
            ops = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (ops == 0) ops = 10000;

    Harness h;
    constexpr int kPlayers = 1000;
    for (int i = 0; i < kPlayers; ++i) {
        h.EnsureChar(static_cast<PlayerId>(10000 + i));
    }
    // 预置余额，让 Remove/Transfer/Purchase 有成功的样本（失败路径也要纳入，才是真实分布）
    for (int i = 0; i < kPlayers; ++i) {
        EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 10000 + i, 1, "seed" + std::to_string(i));
        c.amount = 1000000;
        (void)h.eco.Execute(c, h.ctx);
    }
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    std::uint64_t txn = 1000000;

    // ---- 1) execute_ns：混合八种操作 ----
    std::uint64_t seed = 20260910;
    auto rnd = [&seed]() {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(seed >> 33);
    };
    std::size_t applied = 0;
    const auto t_exec = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < ops; ++i) {
        const std::uint32_t pick = rnd() % 100;
        const PlayerId p = 10000 + (rnd() % kPlayers);
        EconomyOp op = EconomyOp::AddCurrency;
        if (pick < 25) op = EconomyOp::AddCurrency;
        else if (pick < 45) op = EconomyOp::RemoveCurrency;
        else if (pick < 60) op = EconomyOp::Transfer;
        else if (pick < 75) op = EconomyOp::Purchase;
        else if (pick < 90) op = EconomyOp::Reward;
        else op = EconomyOp::AddItem;

        EconomyCommand c = MakeCmd(op, p, ++txn, "b-" + std::to_string(i));
        if (op == EconomyOp::Transfer) {
            c.peer = 10000 + ((p + 7) % kPlayers);
            c.amount = 1 + (rnd() % 100);
        } else if (op == EconomyOp::Purchase) {
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else if (op == EconomyOp::AddItem) {
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
        } else {
            c.amount = 1 + (rnd() % 200);
        }
        auto r = h.eco.Execute(c, h.ctx);
        if (r.HasValue() && r.Value().applied) ++applied;
    }
    const double execute_ns = static_cast<double>(ElapsedNs(t_exec)) / static_cast<double>(ops);
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    // ---- 2) balance_query_ns：批量摊销（避免把时钟成本算进指标）----
    std::int64_t sink_sum = 0;
    const auto t_bal = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < ops; ++i) {
        const PlayerId p = 10000 + (i % kPlayers);
        auto b = h.eco.Balance(p, kCurrencyGold);
        sink_sum += b.HasValue() ? b.Value() : 0;
    }
    const double balance_query_ns =
        static_cast<double>(ElapsedNs(t_bal)) / static_cast<double>(ops);
    (void)sink_sum;

    // ---- 3) transfer_ns：双人原子转移 ----
    double transfer_ns = 0.0;
    {
        const std::size_t n = ops / 4 > 0 ? ops / 4 : 1;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            const PlayerId p = 10000 + (i % kPlayers);
            EconomyCommand c = MakeCmd(EconomyOp::Transfer, p, ++txn, "bt-" + std::to_string(i));
            c.peer = 10000 + ((i + 1) % kPlayers);
            c.amount = 1;
            (void)h.eco.Execute(c, h.ctx);
        }
        transfer_ns = static_cast<double>(ElapsedNs(t0)) / static_cast<double>(n);
    }
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    // ---- 4) purchase_ns：查价 + 扣币 + 发物品（两步一致性路径）----
    double purchase_ns = 0.0;
    {
        const std::size_t n = ops / 4 > 0 ? ops / 4 : 1;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            const PlayerId p = 10000 + (i % kPlayers);
            EconomyCommand c = MakeCmd(EconomyOp::Purchase, p, ++txn, "bp-" + std::to_string(i));
            c.item_deltas.push_back(ItemDelta{1, 1, kInvalidItemGuid});
            (void)h.eco.Execute(c, h.ctx);
        }
        purchase_ns = static_cast<double>(ElapsedNs(t0)) / static_cast<double>(n);
    }
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    // ---- 5) dedup_check_ns：幂等表命中查询（§22 ≤ 200ns）----
    double dedup_check_ns = 0.0;
    {
        const std::string dup = "bench-dedup-key";
        EconomyCommand first = MakeCmd(EconomyOp::AddCurrency, 10000, ++txn, dup);
        first.amount = 1;
        (void)h.eco.Execute(first, h.ctx);

        const std::size_t n = ops;
        std::size_t hits = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < n; ++i) {
            EconomyCommand c = MakeCmd(EconomyOp::AddCurrency, 10000, ++txn, dup);
            c.amount = 1;
            auto r = h.eco.Execute(c, h.ctx);
            if (r.HasValue() && r.Value().deduplicated) ++hits;
        }
        dedup_check_ns = static_cast<double>(ElapsedNs(t0)) / static_cast<double>(n);
        if (hits != n) LineFmt("WARN: dedup hits=%zu/%zu\n", hits, n);
    }
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

    LineFmt("economy_bench: ops=%zu applied=%zu\n", ops, applied);
    LineFmt("execute_ns=%.3f balance_query_ns=%.3f transfer_ns=%.3f purchase_ns=%.3f "
            "dedup_check_ns=%.3f\n",
            execute_ns, balance_query_ns, transfer_ns, purchase_ns, dedup_check_ns);

    std::error_code ec;
    std::filesystem::create_directories("bench", ec);
    std::ofstream out("bench/economy.txt", std::ios::binary);
    if (!out) {
        LineFmt("FAIL: cannot open bench/economy.txt\n");
        return 1;
    }
    out << "execute_ns=" << execute_ns << "\n";
    out << "balance_query_ns=" << balance_query_ns << "\n";
    out << "transfer_ns=" << transfer_ns << "\n";
    out << "purchase_ns=" << purchase_ns << "\n";
    out << "dedup_check_ns=" << dedup_check_ns << "\n";
    out.close();
    return 0;
}
