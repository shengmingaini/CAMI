/**
 * CAMI Tech Stack Verification — TCP Echo Server
 *
 * Async TCP echo server using boost::asio.
 * Validates: C++17 compilation, boost::asio async I/O, multi-threaded performance.
 *
 * Target: QPS >= 100,000 (echo round-trips per second)
 *
 * Usage: tcp_echo_server [port] [thread_count]
 *   port          — listen port (default: 9999)
 *   thread_count  — io_context threads (default: hardware_concurrency)
 *
 * Build:
 *   mkdir build && cd build
 *   cmake .. -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
 *   cmake --build . --target tcp_echo_server
 *
 * [PROTOTYPE] — Tech verification demo, not production code.
 */

#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <vector>
#include <array>
#include <thread>

using boost::asio::ip::tcp;
using namespace std::chrono;

// ============================================================
// Session — per-connection handler
// ============================================================
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, std::atomic<uint64_t>& echo_count)
        : socket_(std::move(socket)), echo_count_(echo_count) {
        // Disable Nagle for low-latency echo
        boost::system::error_code ec;
        socket_.set_option(tcp::no_delay(true), ec);
    }

    void start() {
        do_read();
    }

    ~Session() {
        boost::system::error_code ec;
        socket_.close(ec);
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(data_),
            [this, self](boost::system::error_code ec, std::size_t length) {
                if (!ec) {
                    echo_count_.fetch_add(1, std::memory_order_relaxed);
                    do_write(length);
                }
                // On error: session destructs via shared_ptr release
            });
    }

    void do_write(std::size_t length) {
        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(data_, length),
            [this, self](boost::system::error_code ec, std::size_t /*written*/) {
                if (!ec) {
                    do_read();
                }
            });
    }

    tcp::socket socket_;
    std::array<char, 8192> data_{};
    std::atomic<uint64_t>& echo_count_;
};

// ============================================================
// EchoServer — acceptor + stats
// ============================================================
class EchoServer {
public:
    EchoServer(boost::asio::io_context& ioc, unsigned short port)
        : acceptor_(ioc, tcp::endpoint(tcp::v4(), port)) {
        acceptor_.set_option(boost::asio::socket_base::reuse_address(true));
        do_accept();
    }

    uint64_t total_echoed() const {
        return echo_count_.load(std::memory_order_relaxed);
    }

private:
    void do_accept() {
        acceptor_.async_accept(
            [this](boost::system::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<Session>(std::move(socket), echo_count_)->start();
                    ++conn_count_;
                }
                do_accept();  // continue accepting
            });
    }

    tcp::acceptor acceptor_;
    std::atomic<uint64_t> echo_count_{0};
    std::atomic<uint32_t> conn_count_{0};
};

// ============================================================
// Main
// ============================================================
static std::atomic<bool> g_running{true};

void signal_handler(int /*sig*/) {
    g_running.store(false, std::memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    unsigned short port = 9999;
    int thread_count = static_cast<int>(std::thread::hardware_concurrency());

    if (argc >= 2) port = static_cast<unsigned short>(std::atoi(argv[1]));
    if (argc >= 3) thread_count = std::atoi(argv[2]);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    boost::asio::io_context ioc;
    EchoServer server(ioc, port);

    // Thread pool
    std::vector<std::thread> threads;
    threads.reserve(thread_count - 1);
    for (int i = 1; i < thread_count; ++i) {
        threads.emplace_back([&ioc] {
            try {
                ioc.run();
            } catch (const std::exception& e) {
                std::cerr << "io_context error: " << e.what() << std::endl;
            }
        });
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  CAMI TCP Echo Server [PROTOTYPE]" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "  Port:    " << port << std::endl;
    std::cout << "  Threads: " << thread_count << std::endl;
    std::cout << "  Buffer:  8192 bytes per session" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;

    // Stats reporting thread
    std::thread stats_thread([&]() {
        uint64_t last_echo = 0;
        auto last_time = steady_clock::now();

        while (g_running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(seconds(1));
            uint64_t current_echo = server.total_echoed();
            auto now = steady_clock::now();
            auto elapsed = duration_cast<milliseconds>(now - last_time).count();

            uint64_t qps = (elapsed > 0)
                ? (current_echo - last_echo) * 1000 / static_cast<uint64_t>(elapsed)
                : 0;

            std::cout << "[stats] total_echoed=" << current_echo
                      << "  qps=" << qps << std::endl;

            last_echo = current_echo;
            last_time = now;
        }
    });

    // Main thread also runs io_context
    try {
        ioc.run();
    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
    }

    g_running.store(false, std::memory_order_relaxed);
    stats_thread.join();

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    std::cout << std::endl;
    std::cout << "Server stopped. Total echoed: " << server.total_echoed() << std::endl;
    return 0;
}
