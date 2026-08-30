// engine/core/src/log/logger.cpp — TASK-002 Logger 门面与后台刷盘线程
//
// 线程模型（§9 / §21）：
//   业务线程：ShouldLog -> 格式化到栈上 scratch -> 无锁入队（队列满则丢弃计数，绝不阻塞）
//   后台线程：出队 -> 派发到所有 sink -> flush（唯一触碰文件 IO 的线程）
// 业务线程通过 Flush() 只等待完成计数，不直接操作 sink，避免把 IO 引入 Tick 热路径。

#include "mmo/core/log/logger.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/log/log_sink.h"
#include "log/async_ring_buffer.h"
#include "log/log_formatter.h"

namespace mmo::core {
namespace {

using SinkList = std::vector<std::shared_ptr<ILogSink>>;

constexpr std::chrono::milliseconds kPumpIdleWait{1};
constexpr std::chrono::seconds kFlushTimeout{5};

/// 后台线程单次批量出队的条数：让整批的 cache miss 重叠（见 AsyncRingBuffer::TryDequeueBatch）。
constexpr std::size_t kDrainBatch = 32;

/// sink 的 flush 节流间隔。
/// 持续高压时若每批都 flush，等价于每条日志一次 fflush（一次 write 系统调用），
/// 会把磁盘 IO 放大成整个日志系统的瓶颈；节流后由 stdio 自己的 4KB 缓冲聚合成块写。
/// 「追平」（本批未取满，说明积压已排空）时不受节流限制，立即 flush 保证低延迟可见。
constexpr std::chrono::milliseconds kSinkFlushInterval{10};

/// 热计数器按缓存行隔离，避免生产者 / 消费者 / Flush 之间互相踩 cache line。
alignas(64) std::atomic<bool> g_initialized{false};
alignas(64) std::atomic<bool> g_running{false};
alignas(64) std::atomic<std::uint64_t> g_enqueued{0};
alignas(64) std::atomic<std::uint64_t> g_dropped{0};
alignas(64) std::atomic<std::uint64_t> g_written{0};
alignas(64) std::atomic<std::uint64_t> g_flushed{0};
/// 后台线程是否正在条件变量上等待；生产者据此决定要不要 notify（避免每条日志一次系统调用）。
alignas(64) std::atomic<bool> g_idle{false};

std::string g_service = "gamenode";

detail::AsyncRingBuffer* g_ring = nullptr;

std::thread g_pump;
std::mutex g_pump_mutex;
std::condition_variable g_pump_cv;

std::mutex g_sink_mutex;
/// sink 快照：后台线程持有 shared_ptr 副本后即可无锁遍历；
/// 注册新 sink 时整体替换快照，不影响正在写的后台线程。
const std::shared_ptr<const SinkList> kEmptySinks = std::make_shared<const SinkList>();
std::shared_ptr<const SinkList> g_sinks = kEmptySinks;

std::uint32_t CurrentThreadId() noexcept {
    static std::atomic<std::uint32_t> g_next{1};
    // 逻辑线程号：首个调用该函数的线程为 1，依次递增。比 OS TID 更短，且跨平台一致。
    thread_local const std::uint32_t id = g_next.fetch_add(1, std::memory_order_relaxed);
    return id;
}

std::int64_t NowWallNs() noexcept {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
}

void DispatchAll(const std::shared_ptr<const SinkList>& sinks, const LogRecord& rec) noexcept {
    for (const auto& sink : *sinks) {
        sink->Write(rec);
    }
}

void FlushAll(const std::shared_ptr<const SinkList>& sinks) noexcept {
    for (const auto& sink : *sinks) {
        sink->Flush();
    }
}

/// 队列溢出告警（§19）：只在后台线程发出，业务线程永不直接写 sink。
/// 溢出期间只产生一条 Warn，避免告警本身把队列再次打满。
void EmitOverflowWarn(const std::shared_ptr<const SinkList>& sinks, std::uint64_t delta,
                      std::uint64_t total) noexcept {
    constexpr std::ptrdiff_t kCap = static_cast<std::ptrdiff_t>(kMaxLogMessage) - 1;
    LogRecord rec{};
    rec.timestamp_ns = NowWallNs();
    rec.level = LogLevel::Warn;
    rec.thread_id = CurrentThreadId();
    rec.service = g_service.c_str();
    rec.module = "log";
    try {
        const auto res = std::format_to_n(
            rec.message, kCap, "log queue overflow: dropped {} message(s), total dropped {}",
            delta, total);
        const std::size_t n =
            static_cast<std::size_t>(res.size < kCap ? res.size : kCap);
        rec.message[n] = '\0';
        rec.message_len = static_cast<std::uint32_t>(n);
        rec.packed_size = static_cast<std::uint32_t>(PackedSize(rec));
        DispatchAll(sinks, rec);
    } catch (...) {
        // 告警失败不得影响日志主流程。
    }
}

/// 后台刷盘线程主循环。
void PumpLoop() noexcept {
    std::shared_ptr<const SinkList> sinks;
    {
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        sinks = g_sinks;
    }
    LogRecord batch[kDrainBatch];
    std::uint64_t last_dropped = 0;
    /// 溢出告警按「突发」收敛：一个突发期（连续多轮都在丢）只发一条 Warn，
    /// 直到出现一轮「没有新增丢弃」才认为突发结束，下次再丢才重新告警（§19）。
    bool overflow_active = false;
    std::size_t last_batch = 0;
    auto last_flush = std::chrono::steady_clock::now();

    for (;;) {
        // 每轮开头刷新 sink 快照：配合 RegisterSink 的 notify，新 sink 几乎立即可见。
        {
            std::lock_guard<std::mutex> lk(g_sink_mutex);
            sinks = g_sinks;
        }

        bool did_work = false;
        std::size_t n = 0;
        while ((n = g_ring->TryDequeueBatch(batch, kDrainBatch)) > 0) {
            for (std::size_t i = 0; i < n; ++i) {
                DispatchAll(sinks, batch[i]);
            }
            g_written.fetch_add(n, std::memory_order_relaxed);
            did_work = true;
            last_batch = n;
        }

        const std::uint64_t dropped = g_dropped.load(std::memory_order_relaxed);
        if (dropped > last_dropped) {
            if (!overflow_active) {
                EmitOverflowWarn(sinks, dropped - last_dropped, dropped);
                overflow_active = true;
                did_work = true;
            }
            last_dropped = dropped;
        } else {
            overflow_active = false;  // 突发结束
        }

        if (did_work) {
            const auto now = std::chrono::steady_clock::now();
            if (last_batch < kDrainBatch || now - last_flush >= kSinkFlushInterval) {
                FlushAll(sinks);
                last_flush = now;
                g_flushed.store(g_written.load(std::memory_order_relaxed),
                                std::memory_order_release);
            }
        }

        if (!g_running.load(std::memory_order_acquire)) {
            break;
        }

        {
            std::unique_lock<std::mutex> lk(g_pump_mutex);
            g_idle.store(true, std::memory_order_release);
            // 有界等待：即使生产者的 notify 丢失，最多延迟 1ms，不会丢日志。
            g_pump_cv.wait_for(lk, kPumpIdleWait);
            g_idle.store(false, std::memory_order_release);
        }
    }

    // 收尾：退出前把剩余日志排空并 flush，保证 Shutdown 不丢数据。
    std::size_t tail = 0;
    while ((tail = g_ring->TryDequeueBatch(batch, kDrainBatch)) > 0) {
        for (std::size_t i = 0; i < tail; ++i) {
            DispatchAll(sinks, batch[i]);
        }
        g_written.fetch_add(tail, std::memory_order_relaxed);
    }
    FlushAll(sinks);
    g_flushed.store(g_written.load(std::memory_order_relaxed), std::memory_order_release);
}

Result<void> InitImpl(LoggerConfig cfg) {
    if (g_initialized.load(std::memory_order_acquire)) {
        return Result<void>::Fail(
            Error(ErrorCode::BUSY, "Logger already initialized", domain::kCore));
    }

    const LogFormat format = cfg.json ? LogFormat::kJson : cfg.format;

    // 先构建 sink：这一步可能失败（目录不存在 / 只读 / 权限不足），
    // 必须在启动线程与分配队列之前完成，保证失败时没有副作用残留（§19）。
    auto sinks = std::make_shared<SinkList>();
    if (cfg.console) {
        sinks->push_back(MakeConsoleSink(cfg.console_color, format));
    }
    if (!cfg.file_path.empty()) {
        auto file_sink = MakeRotatingFileSink(std::move(cfg.file_path), cfg.file_max_size,
                                             cfg.file_max_files, format);
        if (!file_sink) {
            return Result<void>::Fail(std::move(file_sink).Err());
        }
        sinks->push_back(std::move(file_sink).Value());
    }

    std::size_t cap = cfg.queue_capacity < 2 ? 2 : cfg.queue_capacity;
    std::size_t pow2 = 1;
    while (pow2 < cap) {
        pow2 <<= 1;
    }

    g_ring = new (std::nothrow) detail::AsyncRingBuffer(pow2);
    if (g_ring == nullptr) {
        return Result<void>::Fail(
            Error(ErrorCode::INTERNAL_ERROR, "alloc log ring buffer failed", domain::kCore));
    }

    g_service = std::move(cfg.service);
    {
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        g_sinks = sinks;
    }
    Logger::SetLevel(cfg.level);
    g_running.store(true, std::memory_order_release);
    g_initialized.store(true, std::memory_order_release);
    g_pump = std::thread(PumpLoop);
    return Result<void>::Ok();
}

}  // namespace

Result<void> Logger::Init(LoggerConfig cfg) {
    // 项目禁用异常传播：把可能的 bad_alloc 收敛成 Error 返回。
    try {
        return InitImpl(std::move(cfg));
    } catch (const std::bad_alloc&) {
        return Result<void>::Fail(
            Error(ErrorCode::INTERNAL_ERROR, "Logger::Init out of memory", domain::kCore));
    } catch (...) {
        return Result<void>::Fail(
            Error(ErrorCode::INTERNAL_ERROR, "Logger::Init unknown failure", domain::kCore));
    }
}

void Logger::Shutdown() noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) {
        return;
    }
    g_running.store(false, std::memory_order_release);
    g_pump_cv.notify_all();
    if (g_pump.joinable()) {
        g_pump.join();
    }
    delete g_ring;
    g_ring = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        g_sinks = kEmptySinks;
    }
    g_initialized.store(false, std::memory_order_release);
}

bool Logger::IsInitialized() noexcept { return g_initialized.load(std::memory_order_acquire); }

void Logger::RegisterSink(std::shared_ptr<ILogSink> sink) {
    if (!sink) {
        return;
    }
    try {
        auto next = std::make_shared<SinkList>();
        std::lock_guard<std::mutex> lk(g_sink_mutex);
        next->assign(g_sinks->begin(), g_sinks->end());
        next->push_back(std::move(sink));
        g_sinks = next;
    } catch (...) {
        // 注册失败不影响既有 sink。
        return;
    }
    // 唤醒后台线程，让新 sink 在下一次循环开头即被拾取（否则最多延迟 1ms）。
    g_pump_cv.notify_all();
}

void Logger::WriteRaw(LogLevel level, const LogContext& ctx, std::string_view message) noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) {
        return;  // 未初始化：静默丢弃，禁止因此崩溃或阻塞业务
    }

    LogRecord rec{};
    rec.timestamp_ns = NowWallNs();
    rec.level = level;
    rec.thread_id = CurrentThreadId();
    rec.service = g_service.c_str();
    // module 契约：静态存储期、'\0' 结尾（见 log_context.h）。
    rec.module = ctx.module.empty() ? "" : ctx.module.data();
    rec.trace_id = ctx.trace_id;
    rec.request_id = ctx.request_id;
    rec.player_id = ctx.player_id;
    rec.scene_id = ctx.scene_id;

    std::size_t n = message.size();
    if (n >= kMaxLogMessage) {
        n = kMaxLogMessage - 1;  // 截断，绝不堆分配
    }
    if (n > 0) {
        std::memcpy(static_cast<void*>(rec.message), static_cast<const void*>(message.data()), n);
    }
    rec.message[n] = '\0';
    rec.message_len = static_cast<std::uint32_t>(n);
    rec.packed_size = static_cast<std::uint32_t>(PackedSize(rec));

    if (g_ring->TryEnqueue(rec)) {
        g_enqueued.fetch_add(1, std::memory_order_relaxed);
        if (g_idle.load(std::memory_order_acquire)) {
            g_pump_cv.notify_one();
        }
        if (level == LogLevel::Fatal) {
            // Fatal 立即落盘（§19）：业务线程仍只等待计数，不碰文件 IO。
            Logger::Flush();
        }
    } else {
        // 队列满：丢弃并计数，绝不阻塞业务线程（§21 红线）。
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
}

void Logger::Flush() noexcept {
    if (!g_initialized.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t target = g_enqueued.load(std::memory_order_acquire) +
                                 g_dropped.load(std::memory_order_acquire);
    const auto deadline = std::chrono::steady_clock::now() + kFlushTimeout;
    for (;;) {
        const std::uint64_t done = g_flushed.load(std::memory_order_acquire) +
                                   g_dropped.load(std::memory_order_acquire);
        if (done >= target) {
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return;  // 有界等待：后台线程异常时不得把业务线程挂死
        }
        g_pump_cv.notify_all();
        std::this_thread::yield();
    }
}

std::uint64_t Logger::DroppedCount() noexcept {
    return g_dropped.load(std::memory_order_relaxed);
}
std::uint64_t Logger::EnqueuedCount() noexcept {
    return g_enqueued.load(std::memory_order_relaxed);
}
std::uint64_t Logger::WrittenCount() noexcept {
    return g_written.load(std::memory_order_relaxed);
}
std::uint64_t Logger::FlushedCount() noexcept {
    return g_flushed.load(std::memory_order_relaxed);
}

}  // namespace mmo::core
