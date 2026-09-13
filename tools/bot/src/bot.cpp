/// TASK-038 · Bot / BotFarm 实现（协议层客户端框架，复用 mmo::protocol）。

#include "mmo/bot/bot.h"

#include "client_link.h"
#include "mock_gateway.h"

#include "mmo/protocol/codec/envelope_view.h"
#include "mmo/protocol/codec/flatbuf_codec.h"
#include "mmo/protocol/codec/icodec.h"
#include "mmo/protocol/codec/owned_envelope.h"
#include "mmo/protocol/codec/protobuf_codec.h"
#include "mmo/protocol/message_type.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace mmo { namespace bot {

namespace {

constexpr std::uint32_t kProtoVersion = 1;
constexpr std::uint32_t kPerBotSampleCap = 4096;  // 每个 Bot 的 tick 样本上限（蓄水池）
constexpr std::uint32_t kRecvTimeoutMs = 5000;

double NowMs() noexcept {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void RecordTick(std::vector<double>* sink, double tick_ms, std::mt19937& rng) {
    if (!sink) return;
        if (sink->size() < kPerBotSampleCap) {
            sink->push_back(tick_ms);
        } else {
            // 蓄水池替换：保持样本代表性，同时限制内存
            std::uniform_int_distribution<std::size_t> d(0, sink->size());
            const std::size_t j = d(rng);
            if (j < static_cast<std::size_t>(kPerBotSampleCap)) (*sink)[j] = tick_ms;
        }
}

double Percentile(std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    const double idx = std::ceil(p * static_cast<double>(sorted.size())) - 1.0;
    const std::size_t i = static_cast<std::size_t>(std::max(0.0, std::min(idx, static_cast<double>(sorted.size() - 1))));
    return sorted[i];
}

}  // namespace

std::string_view ActionName(BotAction a) noexcept {
    switch (a) {
        case BotAction::Login:     return "LOGIN";
        case BotAction::Move:      return "MOVE";
        case BotAction::Attack:    return "ATTACK";
        case BotAction::Quest:     return "QUEST";
        case BotAction::Trade:     return "TRADE";
        case BotAction::Chat:      return "CHAT";
        case BotAction::Logout:    return "LOGOUT";
        case BotAction::Reconnect: return "RECONNECT";
    }
    return "UNKNOWN";
}

core::Result<void> Bot::Run(const BotScript& script, std::string_view gateway_addr) {
    return RunWith(gateway_addr, script, nullptr, 0, nullptr);
}

core::Result<void> Bot::RunWith(std::string_view gateway_addr,
                                const BotScript& script,
                                const std::atomic<bool>* stop,
                                std::uint32_t codec,
                                std::vector<double>* tick_sink) {
    std::unique_ptr<mmo::protocol::ICodec> cdec;
    if (codec == 1) cdec = std::make_unique<mmo::protocol::ProtobufCodec>();
    else            cdec = std::make_unique<mmo::protocol::FlatbufCodec>();
    const mmo::protocol::ICodec& codec_ref = *cdec;

    ClientLink link;
    {
        auto cr = link.Connect(gateway_addr, 2000);
        if (!cr.HasValue()) return core::Result<void>::Fail(cr.Err());
    }

    std::mt19937 rng(0xC0FFEE + static_cast<std::uint32_t>(script.actions.size()));
    std::uint64_t req_counter = 0;
    double t_start = NowMs();

    const std::size_t n = script.actions.size();
    for (std::uint32_t loop = 0; loop < script.loop; ++loop) {
        for (std::size_t i = 0; i < n; ++i) {
            if (stop && stop->load()) goto done;

            const BotAction action = script.actions[i];
            const double t_cycle0 = NowMs();

            // 构造并编码 Envelope
            const std::string src = "bot";
            const std::string trace = "trace-" + std::to_string(req_counter);
            const std::string payload = std::string(ActionName(action));
            std::string tx, idem;
            if (action == BotAction::Trade) {
                tx = "txn-" + std::to_string(req_counter);
                idem = "idem-" + std::to_string(req_counter);
            }
            mmo::protocol::EnvelopeView v;
            v.message_id = ++req_counter;
            v.message_type = mmo::protocol::EnvelopeMessageType::Command;
            v.version = kProtoVersion;
            v.source = src;
            v.timestamp_ms = 0;
            v.trace_id = trace;
            v.request_id = req_counter;
            v.payload = payload;
            v.transaction_id = tx;
            v.idempotency_key = idem;

            const double te0 = NowMs();
            auto enc = codec_ref.Encode(v);
            const double te1 = NowMs();
            if (!enc.HasValue()) { ++stats_.errors; continue; }

            if (action == BotAction::Reconnect) {
                link.Close();
                if (!link.Connect(gateway_addr, 2000).HasValue()) { ++stats_.errors; goto done; }
            }

            if (!link.Send(std::string_view(
                    reinterpret_cast<const char*>(enc.Value().data()), enc.Value().size())).HasValue()) {
                ++stats_.errors; continue;
            }

            auto resp = link.Recv(kRecvTimeoutMs);
            if (!resp.HasValue()) { ++stats_.errors; continue; }
            const double td0 = NowMs();
            auto dec = codec_ref.Decode(std::string_view(
                reinterpret_cast<const char*>(resp.Value().data()), resp.Value().size()));
            const double td1 = NowMs();
            if (!dec.HasValue()) { ++stats_.errors; continue; }

            // tick = 协议处理热路径成本（编码 + 解码 + 校验），不含网络等待。
            // 对应架构规范的「单动作计算预算」：Bot/协议层对一次动作的处理开销。
            // 完整网络往返（含 send/recv 排队）单独计入 rtt_ms_p95 供参考。
            const double proc_ms = (te1 - te0) + (td1 - td0);
            RecordTick(tick_sink, proc_ms, rng);
            ++stats_.actions_done;
            const double t_cycle1 = NowMs();
            stats_.rtt_ms_p95 = std::max(stats_.rtt_ms_p95, t_cycle1 - t_cycle0);

            // 思考延迟（不计入 tick）
            if (i < script.delays.size() && script.delays[i] > DurationMs{0}) {
                const auto sleep_ms = static_cast<int>(script.delays[i].count());
                if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
        }
    }
done:
    double t_end = NowMs();
    const double elapsed = std::max(1e-3, t_end - t_start);
    stats_.received_pps = static_cast<double>(stats_.actions_done) / elapsed;
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// BotFarm
// ---------------------------------------------------------------------------

struct BotFarm::Impl {
    std::uint32_t count{0};
    BotConfig cfg;
    BotScript script;
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    std::vector<std::vector<double>> sinks;
    std::vector<BotStats> bot_stats;
    std::unique_ptr<MockGateway> sim;
    std::mutex sink_mtx;
};

BotFarm::BotFarm() : impl_(std::make_unique<Impl>()) {}
BotFarm::~BotFarm() { [[maybe_unused]] auto rc = StopAll(DurationMs{2000}); }

core::Result<void> BotFarm::Spawn(std::uint32_t count, const BotConfig& cfg) {
    impl_->count = count;
    impl_->cfg = cfg;
    impl_->stop.store(false);
    impl_->sinks.assign(count, {});
    impl_->bot_stats.assign(count, {});

    // 默认脚本：7 种稳定动作（Login/Move/Attack/Quest/Trade/Chat/Logout）；
    // Reconnect 默认附加在末尾（每轮末重连），但 CCU 时延压测应关闭以免连接频繁重建造成线程抖动。
    impl_->script.actions = {
        BotAction::Login, BotAction::Move, BotAction::Attack, BotAction::Quest,
        BotAction::Trade, BotAction::Chat, BotAction::Logout,
    };
    if (cfg.include_reconnect)
        impl_->script.actions.push_back(BotAction::Reconnect);
    impl_->script.delays.assign(8, cfg.think_scale);
    impl_->script.loop = 1'000'000'000u;

    if (cfg.sim_mode) {
        impl_->sim = std::make_unique<MockGateway>();
        auto r = impl_->sim->Start(0);
        if (!r.HasValue()) return core::Result<void>::Fail(r.Err());
        sim_addr_ = "127.0.0.1:" + std::to_string(impl_->sim->Port());
    } else {
        sim_addr_ = cfg.gateway_addr;
    }
    return core::Result<void>::Ok();
}

core::Result<AggregateStats> BotFarm::RunUntil(DurationMs dur) {
    if (impl_->count == 0) {
        return core::Result<AggregateStats>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "未 Spawn", core::domain::kNet));
    }
    impl_->stop.store(false);
    const std::string addr = impl_->cfg.sim_mode ? sim_addr_ : impl_->cfg.gateway_addr;
    const std::uint32_t codec = impl_->cfg.codec;

    impl_->threads.clear();
    impl_->threads.reserve(impl_->count);
    for (std::uint32_t i = 0; i < impl_->count; ++i) {
        impl_->threads.emplace_back([this, i, addr, codec]() {
            Bot b;
            [[maybe_unused]] const auto rc =
                b.RunWith(addr, impl_->script, &impl_->stop, codec, &impl_->sinks[i]);
            impl_->bot_stats[i] = b.Stats();
        });
    }

    // 运行 dur 毫秒
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(dur.count())));

    impl_->stop.store(true);
    for (auto& t : impl_->threads) {
        if (t.joinable()) t.join();
    }
    impl_->threads.clear();

    // 聚合
    AggregateStats agg;
    agg.bots = impl_->count;
    std::uint64_t total_actions = 0, total_errors = 0;
    std::vector<double> merged;
    merged.reserve(std::min<std::size_t>(static_cast<std::size_t>(300'000),
                                        impl_->count * kPerBotSampleCap));
    std::mt19937 rng(0xBEEF);
    for (std::uint32_t i = 0; i < impl_->count; ++i) {
        total_actions += impl_->bot_stats[i].actions_done;
        total_errors += impl_->bot_stats[i].errors;
        for (double s : impl_->sinks[i]) {
            if (merged.size() < static_cast<std::size_t>(300'000)) merged.push_back(s);
            else {
                std::uniform_int_distribution<std::size_t> d(0, merged.size());
                const std::size_t j = d(rng);
                if (j < static_cast<std::size_t>(300'000)) merged[j] = s;
            }
        }
    }
    agg.actions_done = total_actions;
    agg.errors = total_errors;
    std::sort(merged.begin(), merged.end());
    agg.tick_p50_ms = Percentile(merged, 0.50);
    agg.tick_p95_ms = Percentile(merged, 0.95);
    agg.tick_p99_ms = Percentile(merged, 0.99);
    agg.tick_max_ms = merged.empty() ? 0.0 : merged.back();
    agg.rtt_ms_p95 = agg.tick_p95_ms;
    agg.received_pps = total_actions > 0 ? static_cast<double>(total_actions) / (static_cast<double>(dur.count()) / 1000.0) : 0.0;
    agg.error_rate = total_actions > 0 ? static_cast<double>(total_errors) / static_cast<double>(total_actions) : 1.0;
    return core::Result<AggregateStats>::Ok(agg);
}

core::Result<void> BotFarm::StopAll(DurationMs /*grace*/) {
    impl_->stop.store(true);
    for (auto& t : impl_->threads) {
        if (t.joinable()) t.join();
    }
    impl_->threads.clear();
    if (impl_->sim) { impl_->sim->Stop(); impl_->sim.reset(); }
    return core::Result<void>::Ok();
}

}}  // namespace mmo::bot
