// TASK-003 · Core Time / UUID / Config —— 单元 / 集成 / Failure 测试
//
// 自包含 harness（不依赖 gtest：vcpkg 离线拉不到）。
// 输出一律走 test_print.h，遵守 TASK-000「engine/ 内禁止 cout / printf / cerr」红线。
//
// 覆盖矩阵：
//   §16 单元测试      : TestMonotonic / TestWallClock / TestTickClock / TestUuidFormat /
//                       TestUuidUniqueness / TestUuidV7Ordering / TestConfigBasics
//   §17 集成测试      : TestFakeTickLoop（10s 假 Tick 循环）/ TestConfigHotReloadConcurrency
//   §19 Failure 测试  : TestWallClockRollback / TestUuidEntropyFailure / TestConfigFailure
//   §15.9 注入时钟    : 通过 time_internal::SetInjectedWallClockNanos 模拟 NTP 回拨

#include "test_print.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "mmo/core/config/config_manager.h"
#include "mmo/core/error/error.h"
#include "mmo/core/time/clock.h"
#include "mmo/core/time/tick_clock.h"
#include "mmo/core/time/timer.h"
#include "mmo/core/uuid/uuid.h"

// 白盒内部头（只对本模块测试可见，不进 PUBLIC 接口）
#include "time/wall_clock_seam.h"
#include "uuid/entropy.h"

namespace {

using mmo::core::ConfigManager;
using mmo::core::DurationMs;
using mmo::core::Error;
using mmo::core::ErrorCode;
using mmo::core::ITimerQueue;
using mmo::core::MonotonicClock;
using mmo::core::Result;
using mmo::core::SteadyNs;
using mmo::core::SteadyTime;
using mmo::core::TickClock;
using mmo::core::TimerCallback;
using mmo::core::TimerId;
using mmo::core::TimerKind;
using mmo::core::TimerSpec;
using mmo::core::kNoDeadline;
using mmo::core::Uuid;
using mmo::core::WallClock;

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s\n", __FILE__, __LINE__,     \
                                        #cond);                                        \
            ++g_failures;                                                              \
        }                                                                              \
    } while (false)

// 取值前必须先判 HasValue：Result::Value() 在 Release 下是 std::get，
// 对错误结果会抛 bad_variant_access —— 那样只会得到一个看不懂的崩溃，
// 而不是「哪一行断言失败」。所有取值一律走 EXPECT_OK。
template <typename T>
T CheckOk(const Result<T>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return T{};
    }
    return result.Value();
}

inline bool CheckOk(const Result<void>& result, const char* expr, const char* file, int line) {
    if (!result.HasValue()) {
        ::mmo::core::test::ErrorFmt("FAIL @ %s:%d : %s -> %s\n", file, line, expr,
                                    std::string(result.Err().Message()).c_str());
        ++g_failures;
        return false;
    }
    return true;
}

#define EXPECT_OK(expr) CheckOk((expr), #expr, __FILE__, __LINE__)

namespace tprint = ::mmo::core::test;

void WriteTextFile(const std::string& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    out.flush();
}

// ---- §16 MonotonicClock 单调不回退（100 万次采样） ----------------------

void TestMonotonic() {
    std::int64_t previous = MonotonicClock::Now();
    std::int64_t regressions = 0;
    for (int i = 0; i < 1000000; ++i) {
        const std::int64_t now = MonotonicClock::Now();
        if (now < previous) {
            ++regressions;
        }
        previous = now;
    }
    CHECK(regressions == 0);

    // Point() 与 Now() 同源，同样不得回退
    SteadyTime previous_point = MonotonicClock::Point();
    std::int64_t point_regressions = 0;
    for (int i = 0; i < 200000; ++i) {
        const SteadyTime now = MonotonicClock::Point();
        if (now < previous_point) {
            ++point_regressions;
        }
        previous_point = now;
    }
    CHECK(point_regressions == 0);

    // Elapsed 与 Now 差值量级一致（同一 epoch 下可直接比较）
    const SteadyNs t0 = MonotonicClock::Now();
    const SteadyTime p0 = MonotonicClock::Point();
    const SteadyTime p1 = MonotonicClock::Point();
    const SteadyNs t1 = MonotonicClock::Now();
    const SteadyNs elapsed = MonotonicClock::Elapsed(p0, p1);
    CHECK(elapsed >= 0);
    CHECK(elapsed <= (t1 - t0));
    tprint::LineFmt("[info] monotonic: 1e6 samples, regressions=%lld\n",
                    static_cast<long long>(regressions));
}

void TestWallClock() {
    const std::int64_t nanos = WallClock::UnixNanos();
    const std::int64_t millis = WallClock::UnixMillis();
    // 2020-07-01T00:00:00Z 的 Unix 毫秒
    constexpr std::int64_t kMillis2020 = 1593561600000LL;
    CHECK(millis > kMillis2020);
    // 纳秒与毫秒两种读数必须互相自洽（允许两次读数跨越毫秒边界）
    const std::int64_t skew = (millis * 1000000LL) - nanos;
    CHECK(skew > -2000000LL && skew < 2000000LL);
    tprint::LineFmt("[info] wallclock: unix_millis=%lld\n", static_cast<long long>(millis));
}

// ---- §16 TickClock 基本语义 + §20.2 累计误差 ----------------------------

void TestTickClock() {
    const TickClock clock(20);
    CHECK(clock.Hz() == 20);
    CHECK(clock.TickIntervalNs() == 50000000);
    CHECK(clock.TickInterval() == DurationMs(50));

    const SteadyTime base{};
    auto at = [base](SteadyNs offset_ns) {
        return base + std::chrono::nanoseconds(offset_ns);
    };

    const SteadyTime next = clock.NextTickDeadline(base);
    CHECK(MonotonicClock::Elapsed(base, next) == 50000000);
    CHECK(clock.NextTickDeadline(next) == at(100000000));

    // CatchUpSteps 限幅
    CHECK(clock.CatchUpSteps(at(0), base) == 0);
    CHECK(clock.CatchUpSteps(at(49999999), base) == 0);
    CHECK(clock.CatchUpSteps(at(50000000), base) == 1);
    CHECK(clock.CatchUpSteps(at(99999999), base) == 1);
    CHECK(clock.CatchUpSteps(at(100000000), base) == 2);
    CHECK(clock.CatchUpSteps(at(149999999), base) == 2);
    CHECK(clock.CatchUpSteps(at(1000000000), base) == TickClock::kMaxCatchUpSteps);
    CHECK(clock.CatchUpSteps(at(600000000000LL), base) == TickClock::kMaxCatchUpSteps);
    // 时钟回拨（now < prev）不得产生正数，也不得下溢
    CHECK(clock.CatchUpSteps(at(-5000000), base) == 0);

    // 退化输入：hz=0 与超大 hz 不得除零
    const TickClock zero(0);
    CHECK(zero.Hz() == 1);
    CHECK(zero.TickIntervalNs() == 1000000000);
    const TickClock huge(2000000000U);
    CHECK(huge.TickIntervalNs() >= 1);

    // §20.2：跑 10000 次 Tick，累计误差 < 10ms（整数纳秒累加 -> 理论 0）
    SteadyTime deadline = base;
    for (int i = 0; i < 10000; ++i) {
        deadline = clock.NextTickDeadline(deadline);
    }
    const SteadyNs elapsed = MonotonicClock::Elapsed(base, deadline);
    const SteadyNs expected = 10000LL * 50000000LL;  // 500 s
    const SteadyNs error = (elapsed > expected) ? (elapsed - expected) : (expected - elapsed);
    CHECK(error < 10000000);  // < 10 ms
    tprint::LineFmt("[info] tick drift: 10000 ticks, elapsed=%lldns expected=%lldns error=%lldns\n",
                    static_cast<long long>(elapsed), static_cast<long long>(expected),
                    static_cast<long long>(error));
}

// ---- §19 注入墙钟回拨：TickClock 与 MonotonicClock 不受影响 --------------

void TestWallClockRollback() {
    const std::int64_t baseline = WallClock::UnixNanos();
    const TickClock clock(20);

    // 1) 注入回拨 5 秒，确认注入生效（墙钟确实后退了）
    mmo::core::time_internal::SetInjectedWallClockNanos(baseline - 5000000000LL);
    CHECK(WallClock::UnixNanos() == baseline - 5000000000LL);

    // 2) 回拨期间 MonotonicClock 仍然单调
    const SteadyNs m0 = MonotonicClock::Now();
    const SteadyNs m1 = MonotonicClock::Now();
    CHECK(m1 >= m0);

    // 3) 回拨期间 TickClock deadline 序列严格单调递增
    const SteadyTime start = MonotonicClock::Point();
    SteadyTime deadline = start;
    bool strictly_increasing = true;
    for (int i = 0; i < 200; ++i) {
        if (i == 50) {
            // 中途再次回拨（模拟 NTP 反复校时）
            mmo::core::time_internal::SetInjectedWallClockNanos(baseline - 5000000000LL);
        }
        if (i == 100) {
            // 中途大幅前跳 60 秒（模拟手动改时间）
            mmo::core::time_internal::SetInjectedWallClockNanos(baseline + 60000000000LL);
        }
        const SteadyTime previous = deadline;
        deadline = clock.NextTickDeadline(deadline);
        if (deadline <= previous) {
            strictly_increasing = false;
        }
    }
    mmo::core::time_internal::SetInjectedWallClockNanos(0);  // 关闭注入

    CHECK(strictly_increasing);
    CHECK(MonotonicClock::Elapsed(start, deadline) == 200LL * 50000000LL);
    // 注入关闭后墙钟恢复真实读数
    CHECK(WallClock::UnixNanos() > baseline - 5000000000LL);
    tprint::Line("[info] wallclock rollback: 200 ticks unaffected by -5s / +60s jumps\n");
}

// ---- §15.3 ITimerQueue 接口可编译、可实现（不实现调度） -----------------

class NullTimerQueue final : public ITimerQueue {
public:
    Result<TimerId> Schedule(TimerSpec, TimerCallback) override {
        return Result<TimerId>::Fail(Error(ErrorCode::INTERNAL_ERROR, "timer: no scheduler"));
    }
    Result<void> Cancel(TimerId) override {
        return Result<void>::Fail(Error(ErrorCode::NOT_FOUND, "timer: no scheduler"));
    }
    std::size_t Size() const noexcept override { return 0; }
    std::uint32_t FireReady(SteadyTime) override { return 0; }
    SteadyNs NextDeadlineDelayNs(SteadyTime) const noexcept override { return kNoDeadline; }
};

void TestTimerInterface() {
    static_assert(std::is_abstract_v<ITimerQueue>, "ITimerQueue 必须是纯接口");
    static_assert(std::has_virtual_destructor_v<ITimerQueue>, "ITimerQueue 必须有虚析构");

    const TimerSpec spec{};
    CHECK(spec.kind == TimerKind::OneShot);
    CHECK(spec.first_delay_ns == 0);
    CHECK(spec.interval_ns == 0);
    CHECK(spec.max_fires == 0);

    // 证明接口可被实现（TASK-004 按同一契约落地真实调度）
    NullTimerQueue queue;
    CHECK(queue.Size() == 0);
    CHECK(queue.FireReady(MonotonicClock::Point()) == 0);
    CHECK(queue.NextDeadlineDelayNs(MonotonicClock::Point()) == mmo::core::kNoDeadline);
    const Result<TimerId> scheduled = queue.Schedule(spec, TimerCallback{});
    CHECK(!scheduled.HasValue());
    CHECK(scheduled.Err().Code() == ErrorCode::INTERNAL_ERROR);
    CHECK(!queue.Cancel(1).HasValue());
}

// ---- §16 UUID 格式 / 解析 / 往返 ---------------------------------------

void TestUuidFormat() {
    const Uuid v4 = Uuid::NewV4();
    CHECK(!v4.IsNil());
    CHECK(v4.Version() == 4);
    CHECK(v4.Variant() == 2);

    const std::string text = v4.ToString();
    CHECK(text.size() == 36);
    CHECK(text[8] == '-');
    CHECK(text[13] == '-');
    CHECK(text[18] == '-');
    CHECK(text[23] == '-');

    const Uuid parsed = EXPECT_OK(Uuid::Parse(text));
    CHECK(parsed == v4);
    CHECK(parsed.ToString() == text);

    // 大写形式
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) {
        return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    });
    const Uuid from_upper = EXPECT_OK(Uuid::Parse(upper));
    CHECK(from_upper == v4);

    // 无连字符的 32 字符形式
    std::string compact;
    for (char c : text) {
        if (c != '-') {
            compact.push_back(c);
        }
    }
    CHECK(compact.size() == 32);
    const Uuid from_compact = EXPECT_OK(Uuid::Parse(compact));
    CHECK(from_compact == v4);

    // V7
    const Uuid v7 = Uuid::NewV7();
    CHECK(v7.Version() == 7);
    CHECK(v7.Variant() == 2);
    const std::int64_t wall = WallClock::UnixMillis();
    const std::int64_t embedded = v7.TimestampMillis();
    const std::int64_t gap = (embedded > wall) ? (embedded - wall) : (wall - embedded);
    CHECK(gap < 5000);
    CHECK(EXPECT_OK(Uuid::Parse(v7.ToString())) == v7);

    CHECK(Uuid::Nil().IsNil());

    // §16 非法输入 -> INVALID_ARGUMENT
    const char* const bad_inputs[] = {
        "", "not-a-uuid", "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz",
        "12345678-1234-1234-1234-12345678901",   // 少一位
        "12345678-1234-1234-1234-1234567890123",  // 多一位
        "123456781234-1234-1234-123456789012",    // 连字符位置错
    };
    for (const char* bad : bad_inputs) {
        const Result<Uuid> failed = Uuid::Parse(bad);
        CHECK(!failed.HasValue());
        CHECK(failed.Err().Code() == ErrorCode::INVALID_ARGUMENT);
    }
}

// ---- §16 UUID 唯一性（V4 / V7 各 100 万次） -----------------------------

template <typename Factory>
int CountCollisions(Factory make, int count) {
    std::vector<Uuid> generated;
    generated.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        generated.push_back(make());
    }
    std::sort(generated.begin(), generated.end());
    int collisions = 0;
    for (std::size_t i = 1; i < generated.size(); ++i) {
        if (generated[i] == generated[i - 1]) {
            ++collisions;
        }
    }
    return collisions;
}

void TestUuidUniqueness() {
    const int v4_collisions = CountCollisions([] { return Uuid::NewV4(); }, 1000000);
    CHECK(v4_collisions == 0);
    const int v7_collisions = CountCollisions([] { return Uuid::NewV7(); }, 1000000);
    CHECK(v7_collisions == 0);
    tprint::LineFmt("[info] uuid uniqueness: v4_collisions=%d v7_collisions=%d (1e6 each)\n",
                    v4_collisions, v7_collisions);
}

// ---- §20.3 V7 可按时间排序 ---------------------------------------------

void TestUuidV7Ordering() {
    std::vector<Uuid> generated;
    for (int i = 0; i < 60; ++i) {
        generated.push_back(Uuid::NewV7());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // 生成顺序即时间顺序
    for (std::size_t i = 1; i < generated.size(); ++i) {
        CHECK(generated[i - 1].TimestampMillis() <= generated[i].TimestampMillis());
    }
    // 排序后顺序不变（字节字典序 == 时间序）
    std::vector<Uuid> sorted = generated;
    std::sort(sorted.begin(), sorted.end());
    CHECK(sorted == generated);
}

// ---- §19 熵源失败：返回 INTERNAL_ERROR，绝不降级为弱 UUID ---------------

void TestUuidEntropyFailure() {
    mmo::core::uuid_internal::SetTestFillRandom(
        [](void*, std::size_t) noexcept -> Result<void> {
            return Result<void>::Fail(Error(ErrorCode::INTERNAL_ERROR, "entropy: injected"));
        });

    const Result<Uuid> failed_v4 = Uuid::TryNewV4();
    CHECK(!failed_v4.HasValue());
    CHECK(failed_v4.Err().Code() == ErrorCode::INTERNAL_ERROR);

    const Result<Uuid> failed_v7 = Uuid::TryNewV7();
    CHECK(!failed_v7.HasValue());
    CHECK(failed_v7.Err().Code() == ErrorCode::INTERNAL_ERROR);

    // 便捷入口无法返回错误，必须给 Nil（显式无效），而不是弱随机 UUID
    CHECK(Uuid::NewV4().IsNil());
    CHECK(Uuid::NewV7().IsNil());

    mmo::core::uuid_internal::SetTestFillRandom(nullptr);
    CHECK(!Uuid::NewV4().IsNil());
    CHECK(!Uuid::NewV7().IsNil());
    tprint::Line("[info] uuid entropy failure: TryNew* -> INTERNAL_ERROR, New* -> Nil\n");
}

// ---- §20.6 配置加载与类型校验 ------------------------------------------

void TestConfigBasics() {
    ConfigManager::ResetForTest();
    const Result<void> loaded = ConfigManager::LoadDir("config");
    if (!loaded.HasValue()) {
        tprint::ErrorFmt("FAIL: LoadDir(config) -> %s\n",
                         std::string(loaded.Err().Message()).c_str());
        ++g_failures;
        return;
    }

    CHECK(EXPECT_OK(ConfigManager::Get<std::string>("service.name")) == "gamenode");
    CHECK(EXPECT_OK(ConfigManager::Get<std::string>("service.log_level")) == "info");
    CHECK(EXPECT_OK(ConfigManager::Get<bool>("service.hot_reload")));
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tick.hz")) == 20U);
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tick.max_catch_up_steps")) == 3U);
    CHECK(EXPECT_OK(ConfigManager::Get<std::string>("network.gateway_host")) == "127.0.0.1");
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("network.gateway_port")) == 9000U);
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("network.listen_backlog")) == 512U);
    CHECK(EXPECT_OK(ConfigManager::Get<std::string>("network.protocols[0]")) == "tcp");
    CHECK(EXPECT_OK(ConfigManager::Get<std::string>("network.protocols[1]")) == "quic");
    CHECK(ConfigManager::Contains("tick.hz"));
    CHECK(ConfigManager::Size() == 10);
    CHECK(ConfigManager::Keys().size() == 10);

    // §15.6 缺失 key -> NOT_FOUND 且带 key 名
    const Result<std::uint32_t> missing = ConfigManager::Get<std::uint32_t>("tick.nope");
    CHECK(!missing.HasValue());
    CHECK(missing.Err().Code() == ErrorCode::NOT_FOUND);
    CHECK(missing.Err().Message().find("tick.nope") != std::string_view::npos);

    // §15.6 类型不匹配 -> INVALID_ARGUMENT
    const Result<std::uint32_t> bad_type = ConfigManager::Get<std::uint32_t>("service.name");
    CHECK(!bad_type.HasValue());
    CHECK(bad_type.Err().Code() == ErrorCode::INVALID_ARGUMENT);
    CHECK(bad_type.Err().Message().find("service.name") != std::string_view::npos);

    // 各类转换
    CHECK(EXPECT_OK(ConfigManager::Get<bool>("service.hot_reload")));
    CHECK(EXPECT_OK(ConfigManager::Get<std::int64_t>("tick.hz")) == 20);
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint64_t>("tick.hz")) == 20U);
    CHECK(EXPECT_OK(ConfigManager::Get<std::int32_t>("tick.hz")) == 20);
    CHECK(EXPECT_OK(ConfigManager::Get<double>("tick.hz")) > 19.9);

    // 版本递增 + Set 覆盖 + Reload 回滚到文件值
    const std::uint64_t version_before = ConfigManager::Version();
    CHECK(ConfigManager::Set("tick.hz", "30").HasValue());
    CHECK(ConfigManager::Version() == version_before + 1);
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tick.hz")) == 30U);
    CHECK(ConfigManager::Reload().HasValue());
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tick.hz")) == 20U);
    tprint::LineFmt("[info] config: keys=%zu version=%llu\n", ConfigManager::Size(),
                    static_cast<unsigned long long>(ConfigManager::Version()));
}

// ---- §19 配置失败：损坏文件 / 目录消失 / 重复键 ------------------------

void TestConfigFailure() {
    std::filesystem::create_directories("bench");
    ConfigManager::ResetForTest();
    CHECK(ConfigManager::LoadDir("config").HasValue());
    const std::uint64_t stable_version = ConfigManager::Version();

    // 1) 损坏 JSON -> INVALID_ARGUMENT，旧快照原样保留
    WriteTextFile("bench/bad_config.json", "{ \"tick\": { \"hz\": 20 ,, }");
    const Result<void> corrupt = ConfigManager::LoadFile("bench/bad_config.json");
    CHECK(!corrupt.HasValue());
    CHECK(corrupt.Err().Code() == ErrorCode::INVALID_ARGUMENT);
    CHECK(ConfigManager::Version() == stable_version);
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tick.hz")) == 20U);

    // 2) 文件不存在 -> NOT_FOUND
    const Result<void> absent = ConfigManager::LoadFile("bench/definitely_missing.json");
    CHECK(!absent.HasValue());
    CHECK(absent.Err().Code() == ErrorCode::NOT_FOUND);

    // 3) 目录不存在 -> NOT_FOUND
    const Result<void> no_dir = ConfigManager::LoadDir("bench/definitely_missing_dir");
    CHECK(!no_dir.HasValue());
    CHECK(no_dir.Err().Code() == ErrorCode::NOT_FOUND);

    // 4) 重复键（同一目录内两文件定义同一 key）-> INVALID_ARGUMENT
    std::filesystem::create_directories("bench/dup_cfg");
    WriteTextFile("bench/dup_cfg/a.json", "{\"dup\":{\"v\":1}}");
    WriteTextFile("bench/dup_cfg/b.json", "{\"dup\":{\"v\":2}}");
    const Result<void> duplicated = ConfigManager::LoadDir("bench/dup_cfg");
    CHECK(!duplicated.HasValue());
    CHECK(duplicated.Err().Code() == ErrorCode::INVALID_ARGUMENT);
    CHECK(duplicated.Err().Message().find("dup.v") != std::string_view::npos);

    // 5) 配置目录被删除：Reload 返回 NOT_FOUND，服务继续用旧配置
    std::filesystem::create_directories("bench/tmp_cfg");
    WriteTextFile("bench/tmp_cfg/x.json", "{\"tmp\":{\"v\":7}}");
    CHECK(ConfigManager::LoadFile("bench/tmp_cfg/x.json").HasValue());
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tmp.v")) == 7U);
    std::filesystem::remove_all("bench/tmp_cfg");
    const Result<void> reloaded = ConfigManager::Reload();
    CHECK(!reloaded.HasValue());
    CHECK(reloaded.Err().Code() == ErrorCode::NOT_FOUND);
    // 旧配置仍然可读（禁止半替换）
    CHECK(EXPECT_OK(ConfigManager::Get<std::uint32_t>("tick.hz")) == 20U);
    CHECK(EXPECT_OK(ConfigManager::Get<std::string>("service.name")) == "gamenode");

    std::filesystem::remove_all("bench/dup_cfg");
    std::filesystem::remove("bench/bad_config.json");
    ConfigManager::ResetForTest();
    tprint::Line("[info] config failure: corrupt / missing / duplicate / dir-gone all handled\n");
}

// ---- §17 + §20.4 热更期间并发读安全（4 读线程 × 100 万次读） ------------

void TestConfigHotReloadConcurrency() {
    std::filesystem::create_directories("bench");
    WriteTextFile("bench/reload_a.json", "{\"probe\":{\"hz\":20}}");
    WriteTextFile("bench/reload_b.json", "{\"probe\":{\"hz\":30}}");

    ConfigManager::ResetForTest();
    CHECK(ConfigManager::LoadFile("bench/reload_a.json").HasValue());

    std::atomic<bool> stop{false};
    std::atomic<long long> anomalies{0};
    std::atomic<long long> reads{0};

    std::thread writer([&stop, &anomalies, &reads] {
        int round = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const char* path = ((round & 1) == 0) ? "bench/reload_b.json" : "bench/reload_a.json";
            const Result<void> applied = ConfigManager::LoadFile(path);
            if (!applied.HasValue()) {
                anomalies.fetch_add(1, std::memory_order_relaxed);
            }
            ++round;
            if (round > 200000) {
                stop.store(true, std::memory_order_relaxed);
                break;
            }
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&stop, &anomalies, &reads] {
            long long pending = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                const Result<std::uint32_t> value = ConfigManager::Get<std::uint32_t>("probe.hz");
                if (!value.HasValue()) {
                    anomalies.fetch_add(1, std::memory_order_relaxed);
                } else {
                    const std::uint32_t hz = value.Value();
                    // 快照必须是完整的：只能是 20 或 30，绝不可能是半更新值
                    if (hz != 20U && hz != 30U) {
                        anomalies.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                ++pending;
                if (pending >= 4096) {
                    reads.fetch_add(pending, std::memory_order_relaxed);
                    pending = 0;
                }
            }
            reads.fetch_add(pending, std::memory_order_relaxed);
        });
    }

    // 读满 100 万次后停止写线程
    while (reads.load(std::memory_order_relaxed) < 1000000 &&
           !stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    for (std::thread& reader : readers) {
        reader.join();
    }

    CHECK(anomalies.load() == 0);
    CHECK(reads.load() >= 1000000);
    tprint::LineFmt("[info] config hot reload: reads=%lld anomalies=%lld (4 readers)\n",
                    reads.load(), anomalies.load());

    std::filesystem::remove("bench/reload_a.json");
    std::filesystem::remove("bench/reload_b.json");
    ConfigManager::ResetForTest();
}

// ---- §17 假 Tick 循环：10 秒 20Hz，实测 200 ± 2 且无累积漂移 -----------

void TestFakeTickLoop() {
    const TickClock clock(TickClock::kDefaultHz);
    const SteadyTime start = MonotonicClock::Point();
    const SteadyTime end = start + std::chrono::seconds(10);
    SteadyTime deadline = clock.NextTickDeadline(start);
    std::uint64_t ticks = 0;

    while (MonotonicClock::Point() < end) {
        if (MonotonicClock::Point() >= deadline) {
            ++ticks;
            deadline = clock.NextTickDeadline(deadline);
            continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK(ticks >= 198);
    CHECK(ticks <= 202);
    // deadline 完全由整数纳秒累加推出，累计漂移必须严格为 0
    const SteadyNs expected =
        static_cast<SteadyNs>(ticks + 1) * clock.TickIntervalNs();
    const SteadyNs actual = MonotonicClock::Elapsed(start, deadline);
    CHECK(actual == expected);
    tprint::LineFmt("[info] fake tick loop: ticks=%llu in 10s, drift=%lldns\n",
                    static_cast<unsigned long long>(ticks),
                    static_cast<long long>(actual - expected));
}

}  // namespace

int main() {
    tprint::Line("=== Core Time / UUID / Config (TASK-003) ===\n");
    TestMonotonic();
    TestWallClock();
    TestTickClock();
    TestWallClockRollback();
    TestTimerInterface();
    TestUuidFormat();
    TestUuidUniqueness();
    TestUuidV7Ordering();
    TestUuidEntropyFailure();
    TestConfigBasics();
    TestConfigFailure();
    TestConfigHotReloadConcurrency();
    TestFakeTickLoop();

    if (g_failures == 0) {
        tprint::Line("ALL CORE_TIME TESTS PASSED\n");
        return 0;
    }
    tprint::ErrorFmt("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
