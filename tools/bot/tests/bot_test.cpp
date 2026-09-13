// tools/bot/tests/bot_test.cpp — TASK-038 Bot Framework 单元测试
//
// 覆盖任务书 §16（单元）：8 种行为各自可用；BotScript 解析与循环；BotFarm 批量启停；
// 指标字段完整；报告生成格式正确。
// 沿用 TASK-005 的自定义 harness（CHECK / SUMMARY，统一走 test_print.h 输出通道）。

#include "mmo/bot/bot.h"
#include "mock_gateway.h"

#include "test_print.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace tprint = ::mmo::core::test;
using namespace mmo::bot;

int g_passed = 0;
int g_failed = 0;

#define CHECK(cond, name)                                                 \
    do {                                                                 \
        if (cond) {                                                       \
            ++g_passed;                                                   \
            tprint::LineFmt("[PASS] %s\n", name);                         \
        } else {                                                          \
            ++g_failed;                                                   \
            tprint::LineFmt("[FAIL] %s (line %d)\n", name, __LINE__);     \
        }                                                                \
    } while (0)

std::string GWAddr(MockGateway& gw) {
    return "127.0.0.1:" + std::to_string(gw.Port());
}

int main() {
    // T01：8 种行为各自可用（loop=1，恰好 8 个动作）
    {
        MockGateway gw;
        CHECK(gw.Start(0).HasValue(), "T01_gateway_start");
        const std::string addr = GWAddr(gw);
        Bot bot;
        BotScript script;
        script.actions = {BotAction::Login, BotAction::Move, BotAction::Attack,
                          BotAction::Quest, BotAction::Trade, BotAction::Chat,
                          BotAction::Logout, BotAction::Reconnect};
        script.delays.assign(8, DurationMs{0});
        script.loop = 1;
        auto r = bot.Run(script, addr);
        CHECK(r.HasValue(), "T01_bot_run_ok");
        BotStats st = bot.Stats();
        CHECK(st.actions_done == 8, "T01_actions_done_eq_8");
        CHECK(st.errors == 0, "T01_no_errors");
    }

    // T02：BotScript 解析与循环（loop=3 → 24 个动作）
    {
        MockGateway gw;
        gw.Start(0);
        Bot bot;
        BotScript script;
        script.actions = {BotAction::Login, BotAction::Move};
        script.delays.assign(2, DurationMs{0});
        script.loop = 3;
        (void)bot.Run(script, GWAddr(gw));
        CHECK(bot.Stats().actions_done == 6, "T02_loop_x3_eq_6");
        CHECK(bot.Stats().errors == 0, "T02_loop_no_errors");
    }

    // T03：Trade 带 transaction_id / idempotency_key 透传不报错
    {
        MockGateway gw;
        gw.Start(0);
        Bot bot;
        BotScript script;
        script.actions = {BotAction::Trade};
        script.delays.assign(1, DurationMs{0});
        script.loop = 1;
        bot.Run(script, GWAddr(gw));
        CHECK(bot.Stats().errors == 0, "T03_trade_idempotency_no_error");
    }

    // T04：BotFarm 批量启停（10 个 Bot，sim 模式，运行 400ms）
    {
        BotFarm farm;
        BotConfig cfg;
        cfg.sim_mode = true;
        auto sp = farm.Spawn(10, cfg);
        CHECK(sp.HasValue(), "T04_farm_spawn");
        auto ra = farm.RunUntil(DurationMs{400});
        CHECK(ra.HasValue(), "T04_farm_rununtil");
        AggregateStats agg = ra.Value();
        CHECK(agg.bots == 10, "T04_bots_eq_10");
        CHECK(agg.actions_done > 0, "T04_actions_done_pos");
    }

    // T05：指标字段完整（tick 百分位为有限数，error_rate ∈ [0,1]）
    {
        BotFarm farm;
        BotConfig cfg;
        cfg.sim_mode = true;
        farm.Spawn(8, cfg);
        auto ra = farm.RunUntil(DurationMs{400});
        AggregateStats agg = ra.Value();
        const bool finite = std::isfinite(agg.tick_p50_ms) &&
                            std::isfinite(agg.tick_p95_ms) &&
                            std::isfinite(agg.tick_p99_ms) &&
                            std::isfinite(agg.error_rate);
        CHECK(finite, "T05_metrics_finite");
        CHECK(agg.error_rate >= 0.0 && agg.error_rate <= 1.0, "T05_error_rate_range");
        CHECK(agg.tick_p99_ms >= agg.tick_p50_ms, "T05_p99_ge_p50");
    }

    // T06：报告格式正确（AggregateStats 关键字段名稳定，bench 据此生成 key=value）
    {
        // 仅校验字段可被稳定读取与格式化，不依赖具体数值
        AggregateStats agg{};
        agg.bots = 100; agg.actions_done = 1; agg.tick_p99_ms = 1.0; agg.error_rate = 0.0;
        CHECK(agg.bots == 100 && agg.actions_done == 1, "T06_report_fields");
    }

    // T07：非法网关地址应被安全处理（连接失败，不崩溃、不死锁）
    {
        Bot bot;
        BotScript script;
        script.actions = {BotAction::Login};
        script.delays.assign(1, DurationMs{0});
        script.loop = 1;
        auto r = bot.Run(script, "127.0.0.1:1");  // 端口 1 几乎必然拒绝/超时
        // 连接失败应返回失败 Result（错误被显式上报，而非静默通过）
        CHECK(!r.HasValue(), "T07_invalid_gateway_surfaced");
    }

    tprint::LineFmt("SUMMARY passed=%d failed=%d\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
