// 网关压测客户端 [PROTOTYPE]
// 开 N 条长连接，周期性发送心跳帧（经 codec 封装）保活；统计存活与达标数。
// 单客户端进程连单一 server(ip:port) 受本机临时端口数限制（Win 默认 ~1.6 万），
// 全量 5 万需多端口/多机分摊（见压测报告）。本进程用同步 connect/write，适合负载生成。
//
// 用法：gateway_stress_client [--host 127.0.0.1] [--port 7910] [--connections 2000]
//                            [--interval 5000] [--duration 30000]
#include "stress_common.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <thread>
#include <vector>

namespace cg = cami::gateway;
using boost::asio::ip::tcp;
using std::chrono::steady_clock;
using std::chrono::milliseconds;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = cg::stress::kDefaultPort;
    int n = cg::stress::kDefaultConnections;
    auto interval = cg::stress::kHeartbeatInterval;
    auto duration = cg::stress::kDefaultDuration;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--connections" && i + 1 < argc) n = std::atoi(argv[++i]);
        else if (a == "--interval" && i + 1 < argc) interval = milliseconds(std::atoi(argv[++i]));
        else if (a == "--duration" && i + 1 < argc) duration = milliseconds(std::atoi(argv[++i]));
    }

    boost::asio::io_context ioc;  // 仅用于构造 socket；本客户端用同步操作，不 run io_context。
    std::vector<std::shared_ptr<tcp::socket>> socks;
    socks.reserve(static_cast<std::size_t>(n));

    const tcp::endpoint ep(boost::asio::ip::make_address(host), port);
    int connected = 0;
    for (int i = 0; i < n; ++i) {
        try {
            auto s = std::make_shared<tcp::socket>(ioc);
            s->connect(ep);
            socks.push_back(s);
            ++connected;
        } catch (const std::exception& e) {
            // 达到端口/资源上限即停止尝试，报告已达上限（用于发现本机连接天花板）。
            std::fprintf(stderr, "[stress-client] connect #%d failed: %s\n", i, e.what());
            break;
        }
    }
    std::printf("[stress-client] connected=%d / requested=%d\n", connected, n);

    const auto frame = cg::stress::make_heartbeat_frame();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // 周期性向所有存活 socket 发送心跳帧（低频同步写，客户端可接受）。
    std::thread sender([&]() {
        while (!g_stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(interval);
            for (auto& s : socks) {
                if (!s->is_open()) continue;
                boost::system::error_code ec;
                boost::asio::write(*s, boost::asio::buffer(frame), ec);
                if (ec) s->close(ec);  // 服务端已关闭（超时踢线）→ 关闭本地 socket
            }
        }
    });

    const auto start = steady_clock::now();
    while (!g_stop.load(std::memory_order_relaxed) &&
           (steady_clock::now() - start) < duration) {
        std::this_thread::sleep_for(milliseconds(5000));
        int alive = 0;
        for (auto& s : socks) if (s->is_open()) ++alive;
        std::printf("[stress-client] t=%lldms alive=%d\n",
                    static_cast<long long>(std::chrono::duration_cast<milliseconds>(
                        steady_clock::now() - start).count()), alive);
    }

    g_stop.store(true);
    sender.join();
    for (auto& s : socks) {
        boost::system::error_code ec;
        s->close(ec);
    }
    int alive_end = 0;
    for (auto& s : socks) if (s->is_open()) ++alive_end;
    std::printf("[stress-client] stopped. connected=%d alive_at_end=%d\n", connected, alive_end);
    return 0;
}
