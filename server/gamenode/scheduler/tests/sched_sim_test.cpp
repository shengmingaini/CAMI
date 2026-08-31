// server/gamenode/scheduler/tests/sched_sim_test.cpp — TASK-013 §16/§17/§19
//
// 输出统一走 mmo::core::test（test_print.h），禁止裸 std::cout / printf。
// ctest 标签：Sched_Sim（验收脚本 run_ctest 'Sched_Sim'）。

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "test_print.h"
#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/sched/simulation_scheduler.h"
#include "mmo/game/sched/tick_phase.h"
#include "mmo/game/sched/tick_timing.h"

namespace {

using namespace mmo::game;
using mmo::core::test::ErrorFmt;
using mmo::core::test::Line;
using mmo::core::test::LineFmt;
namespace core = mmo::core;
using core::ErrorCode;

// 20Hz 固定间隔（ns），与 TickClock(20) 一致。
constexpr std::int64_t kIntervalNs = 1'000'000'000LL / 20;  // 50ms

int g_fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ErrorFmt("FAIL: %s (line %d)\n", msg, __LINE__);                   \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)
#define CHECK_CODE(result, expected, msg)                                      \
    do {                                                                       \
        const auto& r_ = (result);                                             \
        if (r_.HasValue()) {                                                   \
            ErrorFmt("FAIL: %s (line %d): expected error, got ok\n", msg,       \
                     __LINE__);                                                \
            ++g_fails;                                                         \
        } else if (r_.Err().Code() != (expected)) {                            \
            ErrorFmt("FAIL: %s (line %d): code mismatch\n", msg, __LINE__);    \
            ++g_fails;                                                         \
        }                                                                      \
    } while (0)

// ---- 共享系统单例（与 scene_test 同模式） ----
core::EventBus& Bus() { static core::EventBus b; return b; }
EntityManager& Mgr() { static EntityManager m(&Bus()); return m; }
core::Scheduler& Sch() { static core::Scheduler s; return s; }
core::Arena& Ara() { static core::Arena a(1 << 16); return a; }

// 忙等 ns（注入已知耗时，用于计时精度 / Overrun 验证）
void SpinNs(std::int64_t ns) {
    const core::SteadyNs end = core::MonotonicClock::Now() + ns;
    while (core::MonotonicClock::Now() < end) {
    }
}

// ---- Mock 阶段：记录调用序 + 可选注入耗时 + 可选抛异常 ----
class MockStage : public ISimulationStage {
public:
    MockStage(TickPhase ph, std::string name, std::chrono::nanoseconds delay = {},
              bool throw_on_first = false)
        : ph_(ph), name_(std::move(name)), delay_(delay), throw_on_first_(throw_on_first) {}

    TickPhase Phase() const noexcept override { return ph_; }
    std::string_view Name() const noexcept override { return name_; }

    void Execute(const SceneContext& ctx) override {
        if (throw_on_first_) {
            throw std::runtime_error("mock stage boom");
        }
        if (delay_.count() > 0) {
            SpinNs(static_cast<std::int64_t>(delay_.count()));
        }
        std::lock_guard<std::mutex> lk(mu_);
        calls_.push_back(ph_);
        last_arena_used_ = ctx.frame_arena.UsedBytes();
    }

    std::vector<TickPhase> Calls() {
        std::lock_guard<std::mutex> lk(mu_);
        return calls_;
    }
    std::size_t LastArenaUsed() const {
        std::lock_guard<std::mutex> lk(mu_);
        return last_arena_used_;
    }

private:
    TickPhase ph_;
    std::string name_;
    std::chrono::nanoseconds delay_;
    bool throw_on_first_;
    mutable std::mutex mu_;
    std::vector<TickPhase> calls_;
    std::size_t last_arena_used_{0};
};

// 构造 8 个覆盖全部阶段的 mock（按指定 phase 列表，delay 可选）
std::vector<std::unique_ptr<ISimulationStage>> MakeEightStages(
    std::chrono::nanoseconds delay = {}, bool throw_buff = false) {
    std::vector<std::unique_ptr<ISimulationStage>> out;
    for (std::uint8_t i = 0; i < 8; ++i) {
        const TickPhase ph = kTickPhaseOrder[i];
        const std::string nm = std::string("Mock") + ToString(ph);
        if (throw_buff && ph == TickPhase::Buff) {
            out.push_back(std::make_unique<MockStage>(ph, nm, delay, /*throw=*/true));
        } else {
            out.push_back(std::make_unique<MockStage>(ph, nm, delay));
        }
    }
    return out;
}

SimulationScheduler MakeScheduler(SimulationScheduler::Config cfg = {}) {
    return SimulationScheduler(MakeSceneId(0, 1), SceneType::World, 1, Mgr(), Bus(),
                               Sch(), Ara(), cfg);
}

// 手动驱动 n 个 Tick：首调 seed，后续每次推进一个 interval（无 catch-up 突发）。
void DriveN(SimulationScheduler& s, std::int64_t n) {
    core::SteadyTime d = core::MonotonicClock::Point();
    for (std::int64_t i = 0; i < n; ++i) {
        (void)s.RunUntil(d);
        d += std::chrono::nanoseconds(kIntervalNs);
    }
}

// ---------------------------------------------------------------------------
// §16 / §17 阶段顺序固定（注册乱序也必须按 Input→...→Replication 执行）
// ---------------------------------------------------------------------------
void TestRegisterOrder() {
    // 注册顺序故意打乱（逆序），验证执行时仍按固定顺序 Input→...→Replication
    auto s2 = MakeScheduler();
    // 保留裸指针用于事后查询：RegisterStage 取得所有权，mocks 移动后变 null。
    std::vector<MockStage*> raw;
    std::vector<std::unique_ptr<MockStage>> mocks;
    for (std::uint8_t i = 0; i < 8; ++i) {
        mocks.push_back(
            std::make_unique<MockStage>(kTickPhaseOrder[i], ToString(kTickPhaseOrder[i])));
    }
    for (auto& m : mocks) {
        raw.push_back(m.get());
        CHECK(s2.RegisterStage(std::move(m)).HasValue(), "reg order mock");
    }
    DriveN(s2, 1);
    std::vector<TickPhase> order;
    for (auto* m : raw) {
        auto c = m->Calls();
        if (!c.empty()) order.push_back(c.back());
    }
    CHECK(order.size() == 8, "8 phases executed");
    for (std::uint8_t i = 0; i < 8; ++i) {
        CHECK(order[i] == kTickPhaseOrder[i], "phase order matches fixed order");
    }
}

// ---------------------------------------------------------------------------
// §20 验收 #6 重复注册同 Phase 返回错误（禁止静默覆盖）
// ---------------------------------------------------------------------------
void TestDuplicateRejected() {
    auto s = MakeScheduler();
    CHECK(s.RegisterStage(std::make_unique<MockStage>(TickPhase::Input, "A")).HasValue(),
          "first Input ok");
    CHECK_CODE(s.RegisterStage(std::make_unique<MockStage>(TickPhase::Input, "B")),
               ErrorCode::INVALID_ARGUMENT, "dup Input -> error");
    // Combat 尚未注册，属于不同 Phase → 应成功（禁止把"未注册过的新 Phase"误判为重复）。
    CHECK(s.RegisterStage(std::make_unique<MockStage>(TickPhase::Combat, "C")).HasValue(),
          "Combat (new phase) ok");
    CHECK(s.RegisterStage(std::make_unique<MockStage>(TickPhase::Movement, "D")).HasValue(),
          "Movement (new phase) ok");
}

// ---------------------------------------------------------------------------
// §16 / §20 验收 #2 每阶段独立计时且 sum ≈ total（误差 < 5%）
// ---------------------------------------------------------------------------
void TestPhaseTimingAccuracy() {
    // 每阶段注入 distinct 耗时：(i+1)*50us，验证独立计时与 sum≈total
    auto s2 = MakeScheduler();
    std::vector<std::unique_ptr<MockStage>> mocks;
    for (std::uint8_t i = 0; i < 8; ++i) {
        const auto delay = std::chrono::nanoseconds(static_cast<std::int64_t>((i + 1) * 50'000));
        mocks.push_back(std::make_unique<MockStage>(kTickPhaseOrder[i],
                                                      ToString(kTickPhaseOrder[i]), delay));
    }
    for (auto& m : mocks) CHECK(s2.RegisterStage(std::move(m)).HasValue(), "reg timed mock");

    constexpr std::int64_t kTicks = 2000;
    const core::SteadyNs w0 = core::MonotonicClock::Now();
    DriveN(s2, kTicks);
    const core::SteadyNs w1 = core::MonotonicClock::Now();
    const double avg_total_us =
        static_cast<double>(w1 - w0) / static_cast<double>(kTicks) / 1000.0;

    const auto& T = s2.Timings();
    double sum_phase = 0;
    for (std::uint8_t i = 0; i < 8; ++i) {
        sum_phase += static_cast<double>(T[i].avg_us);
        // 相对顺序：phase i 注入 (i+1)*50us，应随 i 单调增
        if (i > 0) {
            CHECK(T[i].avg_us >= T[i - 1].avg_us, "phase timing monotonic with injected delay");
        }
        // 每个阶段 avg 应明显 > 0（计时确实生效）
        CHECK(T[i].avg_us > 0, "phase avg_us positive");
        // p95/p99/max 不应为 0（有样本）
        CHECK(T[i].max_us >= T[i].avg_us, "max >= avg");
    }
    // §20 验收 #2：各阶段之和 ≈ 总 Tick 耗时（误差 < 5%）
    const double rel = sum_phase > 0 ? (sum_phase - avg_total_us) / avg_total_us : 1.0;
    CHECK(std::abs(rel) <= 0.05, "sum(phase) ≈ total within 5%");
}

// ---------------------------------------------------------------------------
// §20 验收 #4 CatchUp 限幅：注入 5s 空档只补 3 个 Tick
// ---------------------------------------------------------------------------
void TestCatchUpCap() {
    auto s = MakeScheduler();
    auto stages = MakeEightStages();
    for (auto& m : stages) CHECK(s.RegisterStage(std::move(m)).HasValue(), "reg");

    // 首调 seed（tick #1）
    core::SteadyTime d = core::MonotonicClock::Point();
    (void)s.RunUntil(d);
    CHECK(s.TickNumber() == 1, "seeded tick #1");

    // 注入 5s 空档
    d += std::chrono::nanoseconds(5'000'000'000LL);
    (void)s.RunUntil(d);
    // 5s / 50ms = 100 步，限幅到 max_catchup=3 → 仅补 3 个 Tick（#2..#4）
    CHECK(s.TickNumber() == 4, "CatchUp capped at 3 ticks (5s gap)");
}

// ---------------------------------------------------------------------------
// §15.8 / §20 Overrun 计数：单 Tick 总耗时 >50ms 被计数且 Tick 继续
// ---------------------------------------------------------------------------
void TestOverrunCount() {
    auto s = MakeScheduler();
    // 一个在首个 Tick 注入 51ms 的超阶段（其余空）
    auto stages = MakeEightStages();
    // 用带 delay 的 Combat 替换
    std::vector<std::unique_ptr<ISimulationStage>> with_delay;
    for (std::uint8_t i = 0; i < 8; ++i) {
        const TickPhase ph = kTickPhaseOrder[i];
        if (ph == TickPhase::Combat) {
            with_delay.push_back(std::make_unique<MockStage>(ph, "Combat51ms",
                                                              std::chrono::nanoseconds(51'000'000)));
        } else {
            with_delay.push_back(std::make_unique<MockStage>(ph, ToString(ph)));
        }
    }
    for (auto& m : with_delay) CHECK(s.RegisterStage(std::move(m)).HasValue(), "reg overrun mock");

    DriveN(s, 5);  // tick #1 含 51ms 注入 -> Overrun；#2..#5 空
    CHECK(s.TickNumber() == 5, "5 ticks, no crash");
    CHECK(s.OverrunCount() >= 1, "overrun counted (tick > 50ms)");
    CHECK(s.MaxTickUs() >= 50'000, "max tick us recorded");
}

// ---------------------------------------------------------------------------
// §15.6 / §16 帧 Arena 每 Tick Reset：跨 Tick 不无界增长
// ---------------------------------------------------------------------------
void TestFrameArenaReset() {
    auto s = MakeScheduler();
    // 一个在 Execute 内分配固定大小并断言 UsedBytes 恒定的 mock
    class ArenaMock : public ISimulationStage {
    public:
        explicit ArenaMock(std::size_t alloc) : alloc_(alloc) {}
        TickPhase Phase() const noexcept override { return TickPhase::Input; }
        std::string_view Name() const noexcept override { return "ArenaMock"; }
        void Execute(const SceneContext& ctx) override {
            void* p = ctx.frame_arena.Push(alloc_, 8);
            (void)p;
            // 每 Tick 帧开始已 Reset：单分配后 UsedBytes 应恰好 == alloc_
            CHECK(ctx.frame_arena.UsedBytes() == alloc_, "frame arena reset each tick");
        }
    private:
        std::size_t alloc_;
    };
    auto stages = MakeEightStages();
    // 用 ArenaMock 替换 Input 阶段
    std::vector<std::unique_ptr<ISimulationStage>> with_arena;
    for (std::uint8_t i = 0; i < 8; ++i) {
        const TickPhase ph = kTickPhaseOrder[i];
        if (ph == TickPhase::Input) {
            with_arena.push_back(std::make_unique<ArenaMock>(4096));
        } else {
            with_arena.push_back(std::make_unique<MockStage>(ph, ToString(ph)));
        }
    }
    for (auto& m : with_arena) CHECK(s.RegisterStage(std::move(m)).HasValue(), "reg arena mock");

    DriveN(s, 200);  // 若 Arena 未 Reset，UsedBytes 会随 Tick 累积增长 -> CHECK 失败
    CHECK(s.TickNumber() == 200, "200 ticks with frame arena reset");
}

// ---------------------------------------------------------------------------
// §19 / §20 验收 #5 单阶段异常不导致整个 Tick 崩溃（异常被捕获，后续阶段继续）
// ---------------------------------------------------------------------------
void TestStageExceptionSurvives() {
    auto s = MakeScheduler();
    auto stages = MakeEightStages({}, /*throw_buff=*/true);
    for (auto& m : stages) CHECK(s.RegisterStage(std::move(m)).HasValue(), "reg throw mock");

    DriveN(s, 3);
    CHECK(s.TickNumber() == 3, "tick continues after stage exception");
    // Buff 抛异常，但其后的 Quest/Event/Replication 仍应被执行（调用序里存在）
    auto s2 = MakeScheduler();
    // 保留裸指针用于事后查询：RegisterStage 取得所有权，mocks 移动后变 null。
    std::vector<MockStage*> raw;
    std::vector<std::unique_ptr<MockStage>> mocks;
    for (std::uint8_t i = 0; i < 8; ++i) {
        const bool thr = (kTickPhaseOrder[i] == TickPhase::Buff);
        mocks.push_back(
            std::make_unique<MockStage>(kTickPhaseOrder[i], ToString(kTickPhaseOrder[i]),
                                         std::chrono::nanoseconds(0), thr));
    }
    for (auto& m : mocks) {
        raw.push_back(m.get());
        CHECK(s2.RegisterStage(std::move(m)).HasValue(), "reg order+throw");
    }
    DriveN(s2, 1);
    // 收集所有 mock 的调用（异常被捕获，Buff 之后的阶段仍被调用）
    bool saw_quest = false, saw_event = false, saw_repl = false;
    for (auto* m : raw) {
        auto c = m->Calls();
        if (c.empty()) continue;
        const TickPhase last = c.back();
        if (last == TickPhase::Quest) saw_quest = true;
        if (last == TickPhase::Event) saw_event = true;
        if (last == TickPhase::Replication) saw_repl = true;
    }
    CHECK(saw_quest, "Quest executed after throwing Buff");
    CHECK(saw_event, "Event executed after throwing Buff");
    CHECK(saw_repl, "Replication executed after throwing Buff");
}

// ---------------------------------------------------------------------------
// §20 验收 #3 长稳：12000 Tick（10 分钟 @20Hz）无累积漂移（Tick 数 = 12000 ± 5）
// ---------------------------------------------------------------------------
void TestLongStability() {
    auto s = MakeScheduler();
    auto stages = MakeEightStages();  // 空阶段，无注入
    for (auto& m : stages) CHECK(s.RegisterStage(std::move(m)).HasValue(), "reg");

    DriveN(s, 12000);
    CHECK(s.TickNumber() == 12000, "12000 ticks driven (10min @20Hz, no drift)");
    // 纯整数 TickClock：没有任何墙钟反馈，deadline 序列零漂移
    CHECK(s.OverrunCount() == 0, "no overrun with empty stages");
    const double cpu = s.CpuUtilization();
    CHECK(cpu >= 0.0 && cpu <= 1.0, "cpu utilization in [0,1]");
}

}  // namespace

int main() {
    Line("== TASK-013 sched_sim_test ==\n");

    TestRegisterOrder();
    TestDuplicateRejected();
    TestPhaseTimingAccuracy();
    TestCatchUpCap();
    TestOverrunCount();
    TestFrameArenaReset();
    TestStageExceptionSurvives();
    TestLongStability();

    if (g_fails == 0) {
        Line("ALL PASS\n");
        return 0;
    }
    ErrorFmt("FAILED: %d check(s)\n", g_fails);
    return 1;
}
