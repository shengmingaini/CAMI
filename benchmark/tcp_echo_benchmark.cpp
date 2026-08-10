/**
 * CAMI Tech Stack Verification — TCP Echo Benchmark Client
 *
 * Multi-connection async benchmark client for the TCP echo server.
 * Measures round-trip QPS (echo completions per second).
 *
 * Usage: tcp_echo_benchmark [host] [port] [connections] [msg_size] [total_msgs] [pipeline]
 *   host       — server host (default: 127.0.0.1)
 *   port       — server port (default: 9999)
 *   connections — concurrent connections (default: 16)
 *   msg_size   — message size in bytes (default: 64)
 *   total_msgs — total messages to send across all connections (default: 1000000)
 *   pipeline   — pipeline depth per connection (default: 8)
 *
 * Build:
 *   cmake --build . --target tcp_echo_benchmark
 *
 * [PROTOTYPE] — Tech verification demo, not production code.
 */

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>
#include <iomanip>
#include <thread>

using boost::asio::ip::tcp;
using namespace std::chrono;

// ============================================================
// BenchmarkSession — one connection with pipelined sends
// ============================================================
class BenchmarkSession : public std::enable_shared_from_this<BenchmarkSession> {
public:
    BenchmarkSession(boost::asio::io_context& ioc,
                     tcp::endpoint endpoint,
                     int msg_size,
                     int pipeline_depth,
                     std::atomic<int64_t>& remaining,
                     std::atomic<uint64_t>& completed)
        : socket_(ioc),
          endpoint_(endpoint),
          msg_size_(msg_size),
          pipeline_depth_(pipeline_depth),
          remaining_(remaining),
          completed_(completed) {
        // Prepare send buffer with deterministic data
        send_buf_.resize(msg_size, 'X');
        recv_buf_.resize(msg_size);
    }

    void start() {
        do_connect();
    }

private:
    void do_connect() {
        auto self = shared_from_this();
        socket_.async_connect(endpoint_, [this, self](boost::system::error_code ec) {
            if (ec) {
                std::cerr << "Connect failed: " << ec.message() << std::endl;
                return;
            }
            boost::system::error_code opt_ec;
            socket_.set_option(tcp::no_delay(true), opt_ec);

            // Fill the pipeline: send up to pipeline_depth messages immediately
            for (int i = 0; i < pipeline_depth_; ++i) {
                try_send();
            }
        });
    }

    void try_send() {
        int64_t prev = remaining_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 0) {
            // No more messages to send — restore the counter
            remaining_.fetch_add(1, std::memory_order_relaxed);
            check_done();
            return;
        }
        do_write();
    }

    void do_write() {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(send_buf_),
            [this, self](boost::system::error_code ec, std::size_t /*written*/) {
                if (ec) {
                    std::cerr << "Write error: " << ec.message() << std::endl;
                    return;
                }
                do_read();
            });
    }

    void do_read() {
        auto self = shared_from_this();
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(recv_buf_),
            [this, self](boost::system::error_code ec, std::size_t /*read*/) {
                if (ec) {
                    std::cerr << "Read error: " << ec.message() << std::endl;
                    return;
                }
                completed_.fetch_add(1, std::memory_order_relaxed);
                in_flight_.fetch_sub(1, std::memory_order_relaxed);
                try_send();
            });
    }

    void check_done() {
        // Called when we've stopped sending; nothing else needed here.
        // The session will be destroyed when the last shared_ptr is released.
    }

    tcp::socket socket_;
    tcp::endpoint endpoint_;
    int msg_size_;
    int pipeline_depth_;
    std::atomic<int64_t>& remaining_;
    std::atomic<uint64_t>& completed_;
    std::atomic<int> in_flight_{0};
    std::string send_buf_;
    std::string recv_buf_;
};

// ============================================================
// Main
// ============================================================
int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    unsigned short port = 9999;
    int connections = 16;
    int msg_size = 64;
    int64_t total_msgs = 1000000;
    int pipeline = 8;

    if (argc >= 2) host = argv[1];
    if (argc >= 3) port = static_cast<unsigned short>(std::atoi(argv[2]));
    if (argc >= 4) connections = std::atoi(argv[3]);
    if (argc >= 5) msg_size = std::atoi(argv[4]);
    if (argc >= 6) total_msgs = std::atoll(argv[5]);
    if (argc >= 7) pipeline = std::atoi(argv[6]);

    std::cout << "============================================" << std::endl;
    std::cout << "  CAMI TCP Echo Benchmark Client [PROTOTYPE]" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "  Target:       " << host << ":" << port << std::endl;
    std::cout << "  Connections:  " << connections << std::endl;
    std::cout << "  Msg size:     " << msg_size << " bytes" << std::endl;
    std::cout << "  Total msgs:   " << total_msgs << std::endl;
    std::cout << "  Pipeline:     " << pipeline << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << std::endl;

    boost::asio::io_context ioc;
    tcp::endpoint endpoint(
        boost::asio::ip::make_address(host), port);

    std::atomic<int64_t> remaining{total_msgs};
    std::atomic<uint64_t> completed{0};

    // Launch sessions
    std::vector<std::shared_ptr<BenchmarkSession>> sessions;
    for (int i = 0; i < connections; ++i) {
        auto s = std::make_shared<BenchmarkSession>(
            ioc, endpoint, msg_size, pipeline, remaining, completed);
        sessions.push_back(s);
        s->start();
    }

    // Thread pool
    int thread_count = static_cast<int>(std::thread::hardware_concurrency());
    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (int i = 1; i < thread_count; ++i) {
        threads.emplace_back([&ioc] { ioc.run(); });
    }

    // Wait for completion with progress reporting
    auto start_time = steady_clock::now();
    uint64_t last_completed = 0;
    auto last_report = start_time;

    while (true) {
        std::this_thread::sleep_for(milliseconds(200));
        uint64_t current = completed.load(std::memory_order_relaxed);
        auto now = steady_clock::now();
        auto elapsed_ms = duration_cast<milliseconds>(now - last_report).count();

        if (elapsed_ms > 0) {
            uint64_t instant_qps = (current - last_completed) * 1000 / static_cast<uint64_t>(elapsed_ms);
            double pct = 100.0 * current / total_msgs;
            std::cout << "\r  progress: " << std::fixed << std::setprecision(1)
                      << pct << "%  |  instant_qps: " << instant_qps
                      << "  |  completed: " << current << "/" << total_msgs
                      << "          " << std::flush;
        }

        last_completed = current;
        last_report = now;

        if (current >= static_cast<uint64_t>(total_msgs)) {
            break;
        }
    }

    auto end_time = steady_clock::now();
    auto total_ms = duration_cast<milliseconds>(end_time - start_time).count();

    ioc.stop();
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    std::cout << std::endl << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "  BENCHMARK RESULTS" << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "  Total messages:  " << completed.load() << std::endl;
    std::cout << "  Total time:      " << total_ms << " ms" << std::endl;

    if (total_ms > 0) {
        uint64_t avg_qps = completed.load() * 1000 / static_cast<uint64_t>(total_ms);
        double avg_latency_us = static_cast<double>(total_ms) * 1000.0 / completed.load();
        double throughput_mbps = static_cast<double>(completed.load()) * msg_size * 2 / 1024.0 / 1024.0 / (total_ms / 1000.0);

        std::cout << "  Average QPS:     " << avg_qps << std::endl;
        std::cout << "  Avg latency:     " << std::fixed << std::setprecision(2)
                  << avg_latency_us << " us/round-trip" << std::endl;
        std::cout << "  Throughput:       " << std::fixed << std::setprecision(2)
                  << throughput_mbps << " MB/s (bidirectional)" << std::endl;
        std::cout << std::endl;

        // Pass/fail
        if (avg_qps >= 100000) {
            std::cout << "  >>> PASS — QPS >= 100,000 <<<" << std::endl;
        } else {
            std::cout << "  >>> FAIL — QPS < 100,000 <<<" << std::endl;
            std::cout << "  Try increasing connections or pipeline depth." << std::endl;
        }
    }
    std::cout << "============================================" << std::endl;

    return 0;
}
