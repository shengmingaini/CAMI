// 网关压测服务端 [PROTOTYPE→优化]
// 真实 socket：用我们的 ConnectionManager（SO_REUSEPORT 多 acceptor 监听）接受并持有连接；
// 每条连接内嵌 FrameDecoder（on_frame 帧级回调，零拷贝、零全局锁）；空闲超时由连接自身
// idle_timer 判定（构造时注入心跳宽限）；HeartbeatManager 仅作 live 统计（注册/注销）。
// 统计：live 连接数、解码帧数、接收字节（帧级累计）、关闭数。不含业务逻辑/持久化。
//
// [2026-08-12 优化] 移除全局 decoders map + dec_mtx 与每帧 hb.mark_activity：
// 消息热路径零锁（帧解码器随连接、心跳随连接、统计随帧）。见 gateway-performance-review.md。
//
// 用法：gateway_stress_server [--host 0.0.0.0] [--port 7910] [--threads 4]
//                             [--hb-timeout 15000] [--duration 30000]
#include "stress_common.h"

#include "gateway/connection/connection_manager.h"
#include "gateway/heartbeat/heartbeat_manager.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

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

    // 连接自身空闲超时 = 心跳宽限（空闲即踢线，零全局锁）。
    cg::connection::ConnectionManager mgr(static_cast<std::size_t>(threads),
                                          cg::stress::kDefaultListenBacklog,
                                          static_cast<std::uint32_t>(hb_timeout.count()));
    // HeartbeatManager 仅作 live 统计（注册/注销，频率低，锁可接受）。
    cg::heartbeat::HeartbeatConfig hbcfg;
    cg::heartbeat::HeartbeatManager hb(hbcfg);

    std::atomic<std::uint64_t> frames_decoded{0};
    std::atomic<std::uint64_t> bytes_recv{0};
    std::atomic<std::uint64_t> closed{0};

    mgr.set_on_accept([&](std::shared_ptr<cg::connection::Connection> c) {
        const std::uint64_t id = c->id();
        // 帧级回调：解码器随连接（线程亲和），零锁；帧视图零拷贝。
        // 闭包捕获裸 this（非 shared_ptr，避免引用环）；读取发生在连接生命周期内，this 必然有效。
        c->set_on_frame([raw = c.get(), &frames_decoded, &bytes_recv](
                            const std::uint8_t* /*p*/, std::size_t n) {
            frames_decoded.fetch_add(1, std::memory_order_relaxed);
            bytes_recv.fetch_add(n, std::memory_order_relaxed);
            (void)raw;
        });
        // live 统计注册（踢线由连接自身 idle_timer 完成，无集中 tick）。
        auto sp = c;
        hb.register_connection(id, steady_clock::now(), nullptr);
        sp->set_on_closed([&hb, id]() { hb.unregister(id); });
    });

    mgr.set_on_connection_closed([&](cg::connection::Connection&) {
        closed.fetch_add(1, std::memory_order_relaxed);
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

    const auto start = steady_clock::now();
    while (!g_stop.load(std::memory_order_relaxed) &&
           (steady_clock::now() - start) < duration) {
        // 1s 粒度轮询，避免超过 duration 仍阻塞在长 sleep。
        std::this_thread::sleep_for(milliseconds(1000));
        if ((steady_clock::now() - start) < milliseconds(5000)) continue;  // 前 5s 不出统计
        const auto live = hb.live_count();
        std::printf("[stress-server] t=%lldms live=%zu frames=%llu bytes=%llu closed=%llu\n",
                    static_cast<long long>(std::chrono::duration_cast<milliseconds>(
                        steady_clock::now() - start).count()),
                    live, static_cast<unsigned long long>(frames_decoded.load()),
                    static_cast<unsigned long long>(bytes_recv.load()),
                    static_cast<unsigned long long>(closed.load()));
    }

    g_stop.store(true);
    mgr.stop();
    std::printf("[stress-server] stopped. final live=%zu frames=%llu bytes=%llu closed=%llu\n",
                hb.live_count(), static_cast<unsigned long long>(frames_decoded.load()),
                static_cast<unsigned long long>(bytes_recv.load()),
                static_cast<unsigned long long>(closed.load()));
    return 0;
}
