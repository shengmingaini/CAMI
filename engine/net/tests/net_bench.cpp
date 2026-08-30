// engine/net/tests/net_bench.cpp — TASK-008 §18 连接基准
//
// 用法：net_bench --connections 10000 --duration 60
// 输出：bench/net_{conns/1000}k.txt（key=value，供验收脚本 assert_metric 解析）
//   指标：conn_count / established_ms / pps / mbps / cpu_percent / rss_mb / per_conn_mem_kb
// 行为：起 N 个真实 TCP 客户端，连接建立后按 20 msg/s/conn 的节奏发送 64B 消息，
//       服务端（INetworkTransport）由主线程 Poll 消费，统计吞吐与资源占用。
//
// 红线：测试目录同样禁止裸 std::cout / printf / std::cerr（TASK-000 §21），
//       输出统一走 test_print.h（stdout）与 std::ofstream（bench 文件）。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>

#include "test_print.h"
#include "mmo/net/transport.h"

namespace {

using namespace mmo::net;
using mmo::core::test::ErrorFmt;
using mmo::core::test::LineFmt;

// ---- 参数解析 ----
struct BenchArgs {
    int connections = 10000;
    int duration_s = 60;
    int threads = 8;
    uint16_t port = 39340;
    bool idle = false;  // idle：连接建立后不发送，只 Poll（测空闲连接每连接内存，§22）
};

bool ParseArgs(int argc, char** argv, BenchArgs& out) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--connections" && i + 1 < argc) {
            out.connections = std::atoi(argv[++i]);
        } else if (arg == "--duration" && i + 1 < argc) {
            out.duration_s = std::atoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            out.threads = std::atoi(argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            out.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "--idle") {
            out.idle = true;
        } else {
            return false;
        }
    }
    return out.connections > 0 && out.duration_s > 0 && out.threads > 0;
}

// ---- 进程资源 ----
struct ProcStats {
    double rss_mb = 0.0;
    double cpu_percent = 0.0;  // user+kernel CPU 时间 / 进程墙钟时间（仅统计两次采样之间）
};

using Clock = std::chrono::steady_clock;

// 进程 CPU 时间（user+kernel，单位 us）。Windows FILETIME 为 100ns 单位。
double ProcessCpuUs() {
    FILETIME create{}, exit{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user)) {
        return 0.0;
    }
    const auto to_us = [](const FILETIME& ft) -> double {
        return (static_cast<double>(ft.dwHighDateTime) * 4294967296.0 +
                static_cast<double>(ft.dwLowDateTime)) / 10.0;  // 100ns -> us
    };
    return to_us(kernel) + to_us(user);
}

ProcStats SampleProcess(double cpu_start_us, Clock::time_point wall_start) {
    ProcStats s;
    HANDLE h = GetCurrentProcess();
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
        s.rss_mb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    const double cpu_us = ProcessCpuUs() - cpu_start_us;
    const double wall_us = static_cast<double>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - wall_start)
            .count());
    if (wall_us > 0) {
        s.cpu_percent = cpu_us / wall_us * 100.0;
    }
    return s;
}

// ---- 客户端批量连接（多线程加速 1K/5K/10K 建立） ----
struct BenchClient {
    SOCKET fd = INVALID_SOCKET;
    ~BenchClient() {
        if (fd != INVALID_SOCKET) {
            closesocket(fd);
        }
    }
};

// 非阻塞 connect + 等待可写（单 fd select，Windows select 上限 64 因此不能批量等）。
// 可写 ≠ 连接成功：必须再取 SO_ERROR 确认握手完成（拒绝/重置也可写）。
bool WaitConnected(SOCKET fd) {
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    timeval tv{2, 0};
    const int rc = select(0, nullptr, &w, nullptr, &tv);
    if (rc != 1) {
        return false;  // 超时或出错
    }
    int soerr = 0;
    int len = sizeof(soerr);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len) != 0) {
        return false;
    }
    return soerr == 0;
}

void ConnectRange(std::vector<BenchClient>& clients, int begin, int end,
                  const char* ip, uint16_t port, std::atomic<int>& failures) {
    for (int i = begin; i < end; ++i) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            ++failures;
            continue;
        }
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(port);
        inet_pton(AF_INET, ip, &sin.sin_addr);
        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
        connect(s, reinterpret_cast<const sockaddr*>(&sin), sizeof(sin));
        if (WaitConnected(s)) {
            clients[i].fd = s;
        } else {
            closesocket(s);
            ++failures;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    BenchArgs args;
    if (!ParseArgs(argc, argv, args)) {
        ErrorFmt("usage: net_bench --connections N --duration S [--threads T] [--port P] [--idle]\n");
        return 2;
    }
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // ---- 服务端配置：backlog 需容纳全部并发握手 ----
    TcpConfig cfg;
    cfg.max_connections = static_cast<std::uint32_t>(args.connections) + 128;
    cfg.backlog = args.connections + 1024;
    cfg.keepalive_idle_s = 0;
    auto transport = CreateTcpTransport(cfg);
    if (!transport || !transport->Listen("127.0.0.1", args.port)) {
        ErrorFmt("listen failed\n");
        return 1;
    }

    // ---- 客户端批量建立连接（多线程并行） ----
    // 关键：客户端 connect 期间服务端必须同步 Poll（accept）。
    // 若等客户端全部连完再 accept，Windows backlog 队列积压 → 新 SYN 被丢弃 →
    // 客户端 select 全部等满超时 → 1000+ 连接"卡死"。200 连接冒烟正常只是因为
    // backlog 未满。
    const auto t_start = std::chrono::steady_clock::now();
    std::vector<BenchClient> clients(static_cast<std::size_t>(args.connections));
    std::atomic<int> conn_failures{0};
    std::atomic<int> threads_done{0};
    const int kThreads = args.threads;
    std::vector<std::thread> threads;
    const int per = (args.connections + kThreads - 1) / kThreads;
    for (int th = 0; th < kThreads; ++th) {
        const int begin = th * per;
        const int end = std::min(begin + per, args.connections);
        if (begin >= end) {
            break;
        }
        const uint16_t port = args.port;
        threads.emplace_back([&clients, &conn_failures, &threads_done, begin, end, port]() {
            ConnectRange(clients, begin, end, "127.0.0.1", port, conn_failures);
            ++threads_done;
        });
    }
    // 服务端 Poll 与客户端 connect 并行：持续 accept，避免 backlog 积压。
    // 直到全部 connect 线程结束。
    std::vector<TransportEvent> events;
    while (threads_done.load() < static_cast<int>(threads.size())) {
        events.clear();
        (void)transport->Poll(std::chrono::milliseconds(10), events);
    }
    for (auto& th : threads) {
        th.join();
    }
    // 收尾：把仍在 backlog 中的已握手连接 accept 干净
    for (int round = 0; round < 200; ++round) {
        events.clear();
        (void)transport->Poll(std::chrono::milliseconds(20), events);
        if (transport->ConnectionCount() >= static_cast<std::size_t>(args.connections) - conn_failures.load()) {
            break;
        }
    }
    const auto established_ms = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_start)
            .count());

    // ---- 主循环：每 50ms 每个客户端发一条 64B，Poll 消费 ----
    const uint8_t msg[64] = {0xAB};
    std::vector<uint8_t> frame(4 + sizeof(msg));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = static_cast<uint8_t>(sizeof(msg));
    std::memcpy(frame.data() + 4, msg, sizeof(msg));

    uint64_t sent_msgs = 0;
    uint64_t recv_msgs = 0;
    uint64_t recv_bytes = 0;
    const auto t_bench = std::chrono::steady_clock::now();
    const double cpu_start_us = ProcessCpuUs();  // bench 段 CPU 基线（排除建立阶段）
    auto last_send = t_bench;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - t_bench).count();
        if (elapsed >= args.duration_s) {
            break;
        }
        // 每 50ms 批量发送一轮（20 msg/s/conn，远低于内核缓冲上限，不触发背压）
        // idle 模式：不发送，只 Poll 保持连接（测空闲连接每连接内存，§22）
        if (!args.idle && now - last_send >= std::chrono::milliseconds(50)) {
            for (auto& c : clients) {
                if (c.fd == INVALID_SOCKET) {
                    continue;
                }
                const int n = send(c.fd, reinterpret_cast<const char*>(frame.data()),
                                   static_cast<int>(frame.size()), 0);
                if (n > 0) {
                    ++sent_msgs;
                }
            }
            last_send = now;
        }
        events.clear();
        (void)transport->Poll(std::chrono::milliseconds(50), events);
        for (auto& ev : events) {
            if (ev.kind == TransportEvent::Kind::Received) {
                ++recv_msgs;
                recv_bytes += ev.data.size();
            }
        }
    }
    const double bench_s = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t_bench)
            .count()) / 1000.0;

    // ---- 指标采样（CPU 只统计 bench 主循环段，排除连接建立阶段） ----
    const ProcStats ps = SampleProcess(cpu_start_us, t_bench);
    const size_t live = transport->ConnectionCount();
    const double pps = bench_s > 0 ? static_cast<double>(recv_msgs) / bench_s : 0.0;
    const double mbps = bench_s > 0 ? static_cast<double>(recv_bytes) / (bench_s * 1e6) : 0.0;
    const double per_conn_kb = live > 0 ? (ps.rss_mb * 1024.0) / static_cast<double>(live) : 0.0;

    // ---- 输出 bench/net_{1k|5k|10k}.txt ----
    std::string suffix = "k";
    if (args.connections % 1000 == 0) {
        suffix = std::to_string(args.connections / 1000) + "k";
    } else {
        suffix = std::to_string(args.connections);
    }
    std::string file = "bench/net_" + suffix + (args.idle ? "_idle.txt" : ".txt");
    {
        std::ofstream ofs(file);
        ofs << "conn_count=" << live << "\n";
        ofs << "established_ms=" << established_ms << "\n";
        ofs << "pps=" << pps << "\n";
        ofs << "mbps=" << mbps << "\n";
        ofs << "cpu_percent=" << ps.cpu_percent << "\n";
        ofs << "rss_mb=" << ps.rss_mb << "\n";
        ofs << "per_conn_mem_kb=" << per_conn_kb << "\n";
    }
    LineFmt("net_bench done: conns=%d live=%zu failures=%d sent=%llu recv=%llu "
            "established_ms=%.1f pps=%.0f mbps=%.2f rss_mb=%.1f per_conn_mem_kb=%.2f -> %s\n",
            args.connections, live, conn_failures.load(),
            static_cast<unsigned long long>(sent_msgs),
            static_cast<unsigned long long>(recv_msgs), established_ms, pps, mbps,
            ps.rss_mb, per_conn_kb, file.c_str());
    return 0;
}
