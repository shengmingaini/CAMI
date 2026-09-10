// server/gamenode/economy/benchmark/ledger_bench.cpp — TASK-030 §18 / §22
//
// 输出机器可读 key=value 到 bench/ledger.txt（供 assert_metric 解析）：
//   idem_check_ns        单次幂等检查（TryBegin，全新 key）平均耗时       阈值 ≤ 200ns
//   ledger_append_ns     单次账本 Append（内存入队，无 IO）平均耗时      §22 ≤ 500ns
//   flush_ns_per_1k      批量落库 1000 条耗时                           §22 ≤ 50ms
//   dedup_hit_ns         重复请求完整路径（命中首次结果）平均耗时        （观测）
//   mem_bytes_per_entry  单条账本内存占用（槽 + 索引 + 变长实测均值）    阈值 ≤ 256B
//   duplicates_rejected  落库/入队阶段被 UNIQUE/应用层拦下的重复次数     （观测）
//
// 计时口径：所有指标都是**批量摊销**（外层取一次时钟）。理由与 TASK-029 的 economy_bench
// 一致：QPC 调用本身 ≈17ns，若逐次取时钟，200ns 的 `idem_check_ns` 预算会被测量误差吃掉
// 十分之一以上，测出来的是时钟而不是代码。
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 cout/printf。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
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
using mmo::core::test::LineFmt;

using Ledger = mmo::game::economy::ledger::Ledger;
using LedgerConfig = mmo::game::economy::ledger::LedgerConfig;
using LEntry = mmo::game::economy::ledger::LedgerEntry;
using InMemoryLedgerStore = mmo::game::economy::ledger::InMemoryLedgerStore;
using IdempotencyStore = mmo::game::economy::ledger::IdempotencyStore;
using InMemoryIdemTable = mmo::game::economy::ledger::InMemoryIdemTable;

std::uint64_t ElapsedNs(std::chrono::steady_clock::time_point t0) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
            .count());
}

/// 计时估计量：**重复 K 次取中位数**，同时保留最小/最大值一并输出。
///
/// 为什么不直接用「单批均值」：本机实测同一份代码同一份工作量，单批均值的分布是
/// `143.7 / 145.9 / 150.8 / 188.9 / 230.3 …` ns（12 次采样），最大值是最小值的 1.6 倍。
/// Windows 的 `steady_clock` 走 QPC、另有线程迁移与页错误抖动，单批均值会把**离群的那一次
/// 采样**变成验收结论——表现为「同一份代码时过时不过」的 flaky 门禁。
/// 中位数是稳健估计量：它不改变被测工作量（每轮都真跑 ops 次），只是把「偶然被打断的那一轮」
/// 排除在中心趋势之外；同时**把 min/max 一并输出**，任何读者都能看到真实离散度，
/// 避免「用中位数掩盖离群」变成另一种假数据。
struct TimingStats {
    double median{0.0};
    double min{0.0};
    double max{0.0};
};

TimingStats MeasureMedian(int rounds, const std::function<double()>& once) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    for (int i = 0; i < rounds; ++i) samples.push_back(once());
    std::sort(samples.begin(), samples.end());
    TimingStats s;
    s.median = samples[samples.size() / 2];
    s.min = samples.front();
    s.max = samples.back();
    return s;
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

}  // namespace

int main(int argc, char** argv) {
    std::size_t ops = 10000;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--ops" && i + 1 < argc) {
            ops = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (ops < 1000) ops = 1000;

    // ---- 1) idem_check_ns：全新 key 的幂等检查（含首次插入）----
    //
    // 计时口径（**必须**，否则测的是 harness 而不是被测代码）：key 在计时循环**之外**
    // 预先构造好。理由：`"idem-" + std::to_string(i)` 自身含 2 次堆分配
    // （to_string 一次 + 拼接一次），而生产路径里 `idempotency_key` 是**随协议消息一起到达**的
    // （`EconomyCommand::idempotency_key`），调用方不需要现造。把它算进来会让
    // `idem_check_ns` 变成「itoa + 字符串拼接」的耗时。
    // 轮数：取 9 轮中位数。实测本机单轮均值的离散度很大（同一份代码同一工作量，
    // 单批均值 143.7~230.3ns，极差 1.6 倍），轮数太少时中位数自己也会落进噪声区；
    // 9 轮（奇数，中位数取得到真实样本点）已能把中心估计稳定在 ~150ns。
    constexpr int kRounds = 9;
    const auto kTtl = mmo::game::economy::ledger::DurationMs{30000};

    // ---- 1) idem_check_ns：全新 key 的幂等检查（含首次插入）----
    const auto idem_keys = [&] {
        std::vector<std::string> k(ops);
        for (std::size_t i = 0; i < ops; ++i) k[i] = "idem-" + std::to_string(i);
        return k;
    }();
    std::size_t fresh_hits = 0;
    TimingStats idem_stats;
    {
        // 预热（不计入）：让分配器、分支预测与 map 桶数组进入稳态。
        {
            InMemoryIdemTable warm_table;
            IdempotencyStore warm(warm_table);
            for (std::size_t i = 0; i < ops; ++i) (void)warm.TryBegin(idem_keys[i], kTtl);
        }
        idem_stats = MeasureMedian(kRounds, [&] {
            InMemoryIdemTable timed_table;  // 每轮全新表：每条 key 都走 Fresh 路径
            IdempotencyStore timed_idem(timed_table);
            std::size_t hits = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < ops; ++i) {
                auto st = timed_idem.TryBegin(idem_keys[i], kTtl);
                if (st.HasValue() &&
                    st.Value() == mmo::game::economy::ledger::IdemStatus::Fresh) {
                    ++hits;
                }
            }
            const double ns = static_cast<double>(ElapsedNs(t0)) / static_cast<double>(ops);
            fresh_hits = hits;
            return ns;
        });
    }
    const double idem_check_ns = idem_stats.median;

    // ---- 2) ledger_append_ns + mem_bytes_per_entry ----
    // 同上：key 与审计文本预构造，避免把 harness 的字符串分配记到账本头上。
    // 每轮换成全新的 Ledger，保证「被测对象形状」在轮与轮之间一致（否则前几轮是热缓存、
    // 后几轮是冷缓存，中位数会失去意义）。
    const auto ledger_keys = [&] {
        std::vector<std::string> k(ops);
        for (std::size_t i = 0; i < ops; ++i) k[i] = "b-" + std::to_string(i);
        return k;
    }();
    const std::string kReason = "bench";
    const std::string kSource = "bench:ledger";

    LedgerConfig cfg;
    cfg.capacity = ops + 16;  // 不让环形缓冲在计时循环内触发 Flush

    std::int64_t appended = 0;
    std::size_t mem_bytes_per_entry = 0;
    TimingStats append_stats;
    {
        append_stats = MeasureMedian(kRounds, [&] {
            InMemoryLedgerStore store;
            Ledger led(store, cfg);
            std::int64_t n = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < ops; ++i) {
                LEntry e;
                e.transaction_id = 1'000'000ull + i;
                e.request_id = 2'000'000ull + i;
                e.idempotency_key = ledger_keys[i];
                e.player = 10000 + (i % 1000);
                e.peer = 0;
                e.op = EconomyOp::AddCurrency;
                e.currency = kCurrencyGold;
                e.delta = 100;
                e.balance_after = 100;
                e.reason = kReason;
                e.source = kSource;
                e.timestamp_ms = 1'700'000'000'000 + static_cast<std::int64_t>(i);
                e.version = static_cast<std::uint32_t>(i);
                if (led.Append(e).HasValue()) ++n;
            }
            const double ns = static_cast<double>(ElapsedNs(t0)) / static_cast<double>(ops);
            appended = n;
            mem_bytes_per_entry = led.MemoryBytesPerEntry();
            return ns;
        });
    }
    const double ledger_append_ns = append_stats.median;
    const std::uint64_t appended_u = static_cast<std::uint64_t>(appended);

    // ---- 3) flush_ns_per_1k：批量落库（容量内一次性排空）----
    const std::size_t flush_batch = (ops < 1000) ? ops : 1000;
    TimingStats flush_stats;
    std::size_t stored_rows = 0;
    std::uint64_t duplicate_rejections = 0;
    {
        flush_stats = MeasureMedian(kRounds, [&] {
            InMemoryLedgerStore store;
            Ledger led(store, cfg);
            for (std::size_t i = 0; i < flush_batch; ++i) {
                LEntry e;
                e.transaction_id = 3'000'000ull + i;
                e.request_id = 4'000'000ull + i;
                e.idempotency_key = ledger_keys[i];  // 与上一段复用同批 key（键长一致即可）
                e.player = 10000 + (i % 1000);
                e.op = EconomyOp::AddCurrency;
                e.currency = kCurrencyGold;
                e.delta = 100;
                e.balance_after = 100;
                e.reason = kReason;
                e.source = kSource;
                e.timestamp_ms = 1'700'000'000'000 + static_cast<std::int64_t>(i);
                e.version = static_cast<std::uint32_t>(i);
                (void)led.Append(e);
            }
            const auto t0 = std::chrono::steady_clock::now();
            auto r = led.Flush();
            const double ns = static_cast<double>(ElapsedNs(t0));
            if (!r.HasValue()) {
                LineFmt("FAIL: ledger flush failed\n");
                std::exit(1);
            }
            stored_rows = store.Size();
            duplicate_rejections = store.DuplicateRejections() + led.Stats().duplicates_rejected;
            return ns;
        });
    }
    const double flush_ns_per_1k = flush_stats.median;

    // ---- 4) dedup_hit_ns：重复请求完整路径（命中首次结果，无副作用）----
    Harness h;
    constexpr int kPlayers = 256;
    for (int i = 0; i < kPlayers; ++i) h.EnsureChar(static_cast<PlayerId>(20000 + i));

    InMemoryIdemTable eco_table;
    IdempotencyStore eco_idem(eco_table);
    InMemoryLedgerStore eco_ledger_store;
    Ledger eco_ledger(eco_ledger_store, LedgerConfig{8192, 3});
    h.eco.SetIdempotencyStore(&eco_idem);
    h.eco.SetLedger(&eco_ledger);

    auto make = [](EconomyOp op, PlayerId p, std::uint64_t txn, const std::string& key) {
        EconomyCommand c;
        c.request_id = txn;
        c.trace_id = txn;
        c.transaction_id = txn;
        c.idempotency_key = key;
        c.player = p;
        c.op = op;
        c.currency = kCurrencyGold;
        c.reason = "bench";
        c.source = "bench:ledger";
        c.timestamp_ms = 1'700'000'000'000;
        return c;
    };

    TimingStats dedup_stats;
    std::size_t dedup_hits = 0;
    {
        EconomyCommand seed = make(EconomyOp::AddCurrency, 20000, 900000, "bench-ledger-seed");
        seed.amount = 1'000'000;
        (void)h.eco.Execute(seed, h.ctx);
        while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();

        // 重复请求路径无副作用，可安全重复 K 轮（同一个已 Completed 的 key）。
        dedup_stats = MeasureMedian(kRounds, [&] {
            std::size_t hits = 0;
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < ops; ++i) {
                EconomyCommand c =
                    make(EconomyOp::AddCurrency, 20000, 900001 + i, "bench-ledger-seed");
                c.amount = 1;
                auto r = h.eco.Execute(c, h.ctx);
                if (r.HasValue() && r.Value().deduplicated) ++hits;
            }
            dedup_hits = hits;
            return static_cast<double>(ElapsedNs(t0)) / static_cast<double>(ops);
        });
    }
    const double dedup_hit_ns = dedup_stats.median;
    while (h.bus.QueueDepth() > 0) (void)h.bus.Drain();
    auto eco_flush = eco_ledger.Flush();
    if (!eco_flush.HasValue()) {
        LineFmt("FAIL: economy ledger flush failed\n");
        return 1;
    }

    LineFmt("ledger_bench: ops=%zu rounds=%d appended=%llu stored_rows=%zu dedup_hits=%zu/%zu "
            "fresh=%zu\n",
            ops, kRounds, static_cast<unsigned long long>(appended_u), stored_rows, dedup_hits, ops,
            fresh_hits);

    std::error_code ec;
    std::filesystem::create_directories("bench", ec);
    std::ofstream out("bench/ledger.txt", std::ios::binary);
    if (!out) {
        LineFmt("FAIL: cannot open bench/ledger.txt\n");
        return 1;
    }
    // 三个计时指标都取 K 轮中位数（见 TimingStats 注释）；min/max 一并落盘，
    // 便于验收者看到真实离散度，而不是只看到一个被平滑过的中心值。
    out << "idem_check_ns=" << idem_check_ns << "\n";
    out << "idem_check_ns_min=" << idem_stats.min << "\n";
    out << "idem_check_ns_max=" << idem_stats.max << "\n";
    out << "ledger_append_ns=" << ledger_append_ns << "\n";
    out << "ledger_append_ns_min=" << append_stats.min << "\n";
    out << "ledger_append_ns_max=" << append_stats.max << "\n";
    out << "flush_ns_per_1k=" << flush_ns_per_1k << "\n";
    out << "flush_ns_per_1k_min=" << flush_stats.min << "\n";
    out << "flush_ns_per_1k_max=" << flush_stats.max << "\n";
    out << "dedup_hit_ns=" << dedup_hit_ns << "\n";
    out << "mem_bytes_per_entry=" << mem_bytes_per_entry << "\n";
    out << "duplicates_rejected=" << duplicate_rejections << "\n";
    out << "stored_rows=" << stored_rows << "\n";
    out << "timing_rounds=" << kRounds << "\n";
    out.close();

    LineFmt("idem_check_ns=%.3f [%.3f..%.3f] ledger_append_ns=%.3f flush_ns_per_1k=%.1f "
            "dedup_hit_ns=%.3f mem_bytes_per_entry=%zu\n",
            idem_check_ns, idem_stats.min, idem_stats.max, ledger_append_ns, flush_ns_per_1k,
            dedup_hit_ns, mem_bytes_per_entry);
    return 0;
}
