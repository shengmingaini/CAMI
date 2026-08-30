#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mmo/core/log/log_context.h"
#include "mmo/core/log/log_level.h"

namespace mmo::core {

/// 单条日志 message 的上限（含结尾 '\0'）。超长部分截断，不做堆分配回退。
inline constexpr std::size_t kMaxLogMessage = 384;

/// 一条日志的完整字段集合（TASK-002 §8，九项固定字段 + thread_id）。
///
/// 设计为纯 POD：可直接放入无锁环形队列，整条日志路径零堆分配（§22）。
/// service / module 为 const char*，指向静态存储期字符串，禁止指向临时对象。
struct LogRecord {
    std::int64_t timestamp_ns{0};  // 墙钟（UTC）纳秒
    LogLevel level{LogLevel::Info};
    std::uint32_t thread_id{0};  // 逻辑线程号（Logger 内部分配，非 OS TID）
    const char* service{""};     // gateway / gamenode / dataservice / control
    const char* module{""};      // 静态字符串
    TraceID trace_id{kInvalidTraceId};
    RequestID request_id{kInvalidRequestId};
    PlayerID player_id{kInvalidPlayerId};
    SceneID scene_id{kInvalidSceneId};
    std::uint32_t message_len{0};  // message 有效长度（不含 '\0'）
    std::uint32_t packed_size{0};  // 有效字节数，见 PackedSize()
    char message[kMaxLogMessage]{};
};

/// message 之前的定长头部大小。
inline constexpr std::size_t kLogRecordHeaderSize = offsetof(LogRecord, message);

/// 有效字节数：只复制「头部 + 实际消息 + '\0'」，避免每条日志都搬运整个 384 字节缓冲。
inline std::size_t PackedSize(const LogRecord& rec) noexcept {
    const std::size_t n = kLogRecordHeaderSize + static_cast<std::size_t>(rec.message_len) + 1;
    return n < sizeof(LogRecord) ? n : sizeof(LogRecord);
}

/// 按 PackedSize 做部分复制：src 中超出 packed_size 的字节是上一轮复用留下的陈值，不复制。
inline void CopyRecord(LogRecord& dst, const LogRecord& src) noexcept {
    std::memcpy(static_cast<void*>(&dst), static_cast<const void*>(&src), PackedSize(src));
}

}  // namespace mmo::core
