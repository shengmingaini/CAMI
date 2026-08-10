// 网关压测服务端 [PROTOTYPE]
// 真实 socket：用我们的 ConnectionManager（SO_REUSEPORT 多 acceptor 监听）接受并持有连接；
// 每条连接 on_data → FrameDecoder 切帧 → 刷新 HeartbeatManager；独立线程周期 tick 踢线。
// 统计：live 连接数、解码帧数、接收字节、踢线数。不含业务逻辑/持久化（符合接入层红线）。
//
// 用法：gateway_stress_server [--host 0.0.0.0] [--port 7910] [--threads 4]
//                             [--hb-timeout 15000] [--duration 30000]
#include "stress_common.h"

#include "gateway/codec/frame_decoder.h"
#include "gateway/connection/connection_manager.h"
#include "gateway/heartbeat/heartbeat_manager.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace cg = cami::gateway;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
    std::string host = "0.0.0.0";
    std::uint16_t port = cg::stress::kDefaultPort;
    int threads = cg::stress::kDefaultThreads;
    auto hb_timeout = cg::stress::kHeartbeatTimeout;
    auto duration = cg::stress::kDefaultDuration;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--threads" && i + 1 < argc) threads = std::atoi(argv[++i]);
        else if (a == "--hb-timeout" && i + 1 < argc) hb_timeout = milliseconds(std::atoi(argv[++i]));
        else if (a == "--duration" && i + 1 < argc) duration = milliseconds(std::atoi(argv[++i]));
    }

    cg::heartbeat::HeartbeatConfig hbcfg;
    hbcfg.heartbeat_timeout = hb_timeout;
    hbcfg.idle_recycle_timeout = cg::stress::kIdleRecycleTimeout;
    hbcfg.scan_interval = cg::stress::kScanInterval;
    cg::heartbeat::HeartbeatManager hb(hbcfg);

    // 每连接一个 FrameDecoder（codec 模块）；on_data 线程与对应连接同线程，无锁竞争内部。
    std::mutex dec_mtx;
    std::unordered_map<std::uint64_t, cg::codec::FrameDecoder> decoders;
    std::atomic<std::uint64_t> frames_decoded{0};
    std::atomic<std::uint64_t> bytes_recv{0};
    std::atomic<std::uint64_t> kicks{0};

    cg::connection::ConnectionManager mgr(static_cast<std::size_t>(threads),
                                          cg::stress::kDefaultListenBacklog);

    mgr.set_on_accept([&](std::shared_ptr<cg::connection::Connection> c) {
        const std::uint64_t id = c->id();
        // on_data 闭包捕获裸 this（非 shared_ptr，避免 Connection→on_data_→shared_ptr 引用环）；
        // 读取发生在连接自身生命周期内，this 必然有效。
        c->set_on_data([raw = c.get(), &dec_mtx, &decoders, &hb, id, &frames_decoded,
                        &bytes_recv](const std::uint8_t* d, std::size_t n) {
            bytes_recv.fetch_add(n, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(dec_mtx);
            cg::codec::FrameDecoder& dec = decoders[id];
            cg::codec::DecodeResult r = dec.consume(d, n, [&](std::vector<std::uint8_t>&&) {
                hb.mark_activity(id, steady_clock::now());
                frames_decoded.fetch_add(1, std::memory_order_relaxed);
            });
            if (r == cg::codec::DecodeResult::kOversized) {
                raw->close_via_executor();  // 失同步：强制关闭（连接无法自恢复）
            } else {
                raw->mark_activity();  // 原始字节到达即视为活动，刷新本连接空闲计时
            }
        });
        // 注册进心跳管理器；踢线闭包捕获 shared_ptr，close 经所属 executor 安全执行。
        auto sp = c;
        hb.register_connection(id, steady_clock::now(),
                               [sp](cg::heartbeat::TimeoutReason) { sp->close_via_executor(); });
    });

    mgr.set_on_connection_closed([&](cg::connection::Connection& c) {
        hb.unregister(c.id());
        std::lock_guard<std::mutex> lk(dec_mtx);
        decoders.erase(c.id());
    });

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    int rc = mgr.start(host, port);
    if (rc != 0) {
        std::cerr << "[stress-server] ConnectionManager start failed: " << rc << "\n";
        return rc;
    }
    std::printf("[stress-server] listening on %s:%u threads=%d hb_timeout=%lldms duration=%lldms\n",
                host.c_str(), port, threads, static_cast<long long>(hb_timeout.count()),
                static_cast<long long>(duration.count()));

    // 独立线程周期 tick 心跳（hb 自带互斥；kick 闭包经 executor 安全 close，无需 io_context）。
    std::thread ticker([&]() {
        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(cg::stress::kScanInterval);
            auto timed = hb.tick(steady_clock::now());
            kicks.fetch_add(timed.size(), std::memory_order_relaxed);
        }
    });

    const auto start = steady_clock::now();
    while (!g_stop.load(std::memory_order_relaxed) &&
           (steady_clock::now() - start) < duration) {
        // 1s 粒度轮询，避免超过 duration 仍阻塞在长 sleep。
        std::this_thread::sleep_for(milliseconds(1000));
        if ((steady_clock::now() - start) < milliseconds(5000)) continue;  // 前 5s 不出统计
        const auto live = hb.live_count();
        std::printf("[stress-server] t=%lldms live=%zu frames=%llu bytes=%llu kicks=%llu\n",
                    static_cast<long long>(std::chrono::duration_cast<milliseconds>(
                        steady_clock::now() - start).count()),
                    live, static_cast<unsigned long long>(frames_decoded.load()),
                    static_cast<unsigned long long>(bytes_recv.load()),
                    static_cast<unsigned long long>(kicks.load()));
    }

    g_stop.store(true);
    ticker.join();
    mgr.stop();
    std::printf("[stress-server] stopped. final live=%zu frames=%llu bytes=%llu kicks=%llu\n",
                hb.live_count(), static_cast<unsigned long long>(frames_decoded.load()),
                static_cast<unsigned long long>(bytes_recv.load()),
                static_cast<unsigned long long>(kicks.load()));
    return 0;
}
