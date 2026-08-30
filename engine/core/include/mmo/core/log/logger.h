#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "mmo/core/error/result.h"
#include "mmo/core/log/log_context.h"
#include "mmo/core/log/log_level.h"
#include "mmo/core/log/log_record.h"
#include "mmo/core/log/log_sink.h"

namespace mmo::core {

/// 默认环形队列容量（必须取 2 的幂，Init 会自动向上取整）。
/// 32768 槽 × sizeof(LogRecord)≈456B ≈ 15MB 常驻，换取的收益是压测尖峰不丢日志。
inline constexpr std::size_t kDefaultQueueCapacity = 32768;

/// Logger 初始化配置。全部字段有缺省值，最小可用配置只需 `LoggerConfig{}`。
struct LoggerConfig {
    /// 服务名，写入每条日志的 service 字段（gateway / gamenode / dataservice / control）。
    std::string service = "gamenode";

    /// 初始输出级别；运行期可用 Logger::SetLevel 调整。
    LogLevel level = kDefaultLogLevel;

    /// 环形队列容量，向上取整到 2 的幂。
    std::size_t queue_capacity = kDefaultQueueCapacity;

    /// 是否输出到控制台（开发期开，生产通常与文件二选一）。
    bool console = true;
    bool console_color = true;

    /// 日志文件路径；空串表示不落文件。
    std::string file_path;
    std::size_t file_max_size = 64u * 1024u * 1024u;
    std::size_t file_max_files = 5;

    /// 输出格式。json=true 为便捷开关，优先级高于 format。
    LogFormat format = LogFormat::kText;
    bool json = false;
};

namespace detail {
/// 当前最小输出级别。**实现细节，仅为让 ShouldLog 保持在头文件内可内联**：
/// 关闭日志时 MMO_LOG 的开销必须≈单次分支（§22 < 5ns），不能承受一次跨 TU 函数调用。
/// 除 Logger::SetLevel / Logger::Level 外，禁止任何代码直接读写它。
inline std::atomic<std::uint8_t> g_min_level{static_cast<std::uint8_t>(kDefaultLogLevel)};
}  // namespace detail

/// 日志门面（静态类，不可实例化）。
///
/// 线程模型（§9）：业务线程只做「ShouldLog 判断 + 格式化 + 无锁入队」，
/// 文件 IO 全部由唯一的后台刷盘线程承担；队列满时丢弃并计数，业务线程永不阻塞。
class Logger final {
public:
    Logger() = delete;

    /// 初始化：构建 sink（可能失败）、分配环形队列、启动后台刷盘线程。
    /// 失败时返回 Error（如日志文件打不开），此时 Logger 保持未初始化状态，所有写调用被静默忽略。
    static Result<void> Init(LoggerConfig cfg);

    /// 优雅退出：排空队列 -> flush 所有 sink -> 停线程 -> 释放队列。可重复调用。
    static void Shutdown() noexcept;

    static bool IsInitialized() noexcept;

    /// 追加一个 sink。线程安全：通过替换快照生效，不影响正在写的后台线程。
    static void RegisterSink(std::shared_ptr<ILogSink> sink);

    static void SetLevel(LogLevel level) noexcept {
        detail::g_min_level.store(static_cast<std::uint8_t>(level), std::memory_order_relaxed);
    }

    static LogLevel Level() noexcept {
        return static_cast<LogLevel>(detail::g_min_level.load(std::memory_order_relaxed));
    }

    /// 级别闸门。**必须先于任何格式化动作调用**（§21 红线）：
    /// 关闭日志时本函数是唯一开销——一次 relaxed 原子读 + 一次比较。
    static bool ShouldLog(LogLevel level) noexcept {
        return static_cast<std::uint8_t>(level) >=
               detail::g_min_level.load(std::memory_order_relaxed);
    }

    /// 写一条带格式化的日志。
    ///
    /// 格式串走 std::format_string：**非法格式串在编译期报错**（§16），
    /// 例如 `Write(INFO, ctx, "id={} {}", 1)` 会直接编译失败，不会拖到运行期。
    /// 格式化目标为栈上 scratch 缓冲，零堆分配；失败（仅资源耗尽）时静默放弃本条。
    template <typename... Args>
    static void Write(LogLevel level, const LogContext& ctx, std::format_string<Args...> fmt,
                      Args&&... args) noexcept {
        char scratch[kMaxLogMessage];
        try {
            constexpr std::ptrdiff_t kCap = static_cast<std::ptrdiff_t>(sizeof(scratch)) - 1;
            const auto res =
                std::format_to_n(scratch, kCap, fmt, std::forward<Args>(args)...);
            const std::size_t n = static_cast<std::size_t>(res.size < kCap ? res.size : kCap);
            scratch[n] = '\0';
            WriteRaw(level, ctx, std::string_view(scratch, n));
        } catch (...) {
            // 格式化异常只可能是资源耗尽；日志系统不得把故障传导给业务。
        }
    }

    /// 写一条已格式化好的日志（无格式串场景，避免 format 开销）。
    static void WriteRaw(LogLevel level, const LogContext& ctx, std::string_view message) noexcept;

    /// 等待「截至调用时点已入队的日志全部落到 sink 并 flush」。
    ///
    /// 业务线程**不直接做文件 IO**：这里只等待后台线程的完成计数，
    /// 等待有界（5 秒）以免后台线程异常时把业务挂死。
    static void Flush() noexcept;

    /// 因队列满被丢弃的日志条数（单调递增）。
    static std::uint64_t DroppedCount() noexcept;
    /// 成功入队的日志条数。
    static std::uint64_t EnqueuedCount() noexcept;
    /// 已派发到 sink 的日志条数。
    static std::uint64_t WrittenCount() noexcept;
    /// 已派发且 sink 已 flush 的日志条数（Flush() 等待的目标）。
    static std::uint64_t FlushedCount() noexcept;
};

}  // namespace mmo::core

/// 统一日志宏。**先判级别，再格式化**（§21 红线，顺序不可颠倒）。
/// level 取 mmo::core::LogLevel；fmt 必须是编译期字面量（格式串编译期校验）。
#define MMO_LOG(level, fmt, ...)                                                          \
    do {                                                                                  \
        if (::mmo::core::Logger::ShouldLog(level)) {                                      \
            ::mmo::core::Logger::Write(level, ::mmo::core::CurrentLogContext(),            \
                                       fmt __VA_OPT__(, ) __VA_ARGS__);                   \
        }                                                                                 \
    } while (0)

#define MMO_LOG_TRACE(fmt, ...) MMO_LOG(::mmo::core::LogLevel::Trace, fmt __VA_OPT__(, ) __VA_ARGS__)
#define MMO_LOG_DEBUG(fmt, ...) MMO_LOG(::mmo::core::LogLevel::Debug, fmt __VA_OPT__(, ) __VA_ARGS__)
#define MMO_LOG_INFO(fmt, ...) MMO_LOG(::mmo::core::LogLevel::Info, fmt __VA_OPT__(, ) __VA_ARGS__)
#define MMO_LOG_WARN(fmt, ...) MMO_LOG(::mmo::core::LogLevel::Warn, fmt __VA_OPT__(, ) __VA_ARGS__)
#define MMO_LOG_ERROR(fmt, ...) MMO_LOG(::mmo::core::LogLevel::Error, fmt __VA_OPT__(, ) __VA_ARGS__)
#define MMO_LOG_FATAL(fmt, ...) MMO_LOG(::mmo::core::LogLevel::Fatal, fmt __VA_OPT__(, ) __VA_ARGS__)
