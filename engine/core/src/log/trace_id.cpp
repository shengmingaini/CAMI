// engine/core/src/log/trace_id.cpp — TASK-002 TraceID / RequestID 生成
//
// TraceID 布局见 include/mmo/core/log/trace_id.h。
// 生成规则（§8）：(node_id << 48) | (timestamp << 16) | counter，
// 禁止随机数；低 48 位整体是一个「单调游标」，保证严格递增且可排序。

#include "mmo/core/log/trace_id.h"

#include <atomic>
#include <chrono>

namespace mmo::core {
namespace {

/// 节点号，默认 1。启动期设置一次。
std::atomic<std::uint64_t> g_node_id{1};

/// 低 48 位单调游标。
/// 每次取 max(当前微秒<<16, 上次游标+1)，因此：
///   - 同一微秒内高频调用 -> 走 +1 分支，序号自增，不重复；
///   - 跨微秒 / 时钟暂停  -> 走 base 分支，时间位前进；
///   - 32 位微秒回绕（约 71.6 分钟）-> 仍走 +1 分支，单调性不破，仅时间位暂时超前。
std::atomic<std::uint64_t> g_cursor{0};

/// RequestID 序号（高 16 位），同一 Trace 内每次派生递增。
std::atomic<std::uint32_t> g_request_seq{0};

std::uint64_t NowMicros() noexcept {
    // 相对进程启动的微秒数：steady_clock 保证单调，不受系统时钟调整影响。
    static const std::chrono::steady_clock::time_point kBase =
        std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::steady_clock::now() - kBase;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
}

}  // namespace

void SetNodeId(std::uint16_t node_id) noexcept {
    g_node_id.store(static_cast<std::uint64_t>(node_id), std::memory_order_relaxed);
}

std::uint16_t NodeId() noexcept {
    return static_cast<std::uint16_t>(g_node_id.load(std::memory_order_relaxed));
}

TraceID NewTraceID() noexcept {
    const std::uint64_t base = (NowMicros() & 0xFFFF'FFFFull) << 16;
    std::uint64_t cur = g_cursor.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    for (;;) {
        next = (cur < base) ? base : ((cur + 1) & kTraceLowMask);
        // CAS 成功时 cur 保持期望值不变，因此返回值必须取 next（本次独占到的游标）。
        if (g_cursor.compare_exchange_weak(cur, next, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
            break;
        }
    }
    const std::uint64_t node = g_node_id.load(std::memory_order_relaxed);
    return (node << 48) | next;
}

RequestID DeriveRequestID(TraceID trace) noexcept {
    const std::uint64_t seq =
        static_cast<std::uint64_t>(g_request_seq.fetch_add(1, std::memory_order_relaxed)) &
        0xFFFFull;
    return (seq << 48) | LowOf(trace);
}

}  // namespace mmo::core
