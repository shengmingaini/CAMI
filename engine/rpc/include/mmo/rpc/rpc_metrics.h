// rpc_metrics.h — 客户端 per-method 指标（TASK-006 §15.8，为 TASK-039 预留）
//
// 键：gRPC Stub 成员函数指针的地址（同一 method 在全进程唯一）。
// TASK-039 指标采集接入时只需定期 Snapshot() 上报，不改变调用路径。
#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace mmo::rpc {

struct RpcMethodSnapshot {
    const void* method{nullptr};   // Stub 成员函数指针地址
    uint64_t    calls{0};          // 总调用次数（含重试的每次尝试）
    uint64_t    errors{0};         // 失败次数
    uint64_t    retries{0};        // 因重试多发出的尝试次数
    uint64_t    total_latency_us{0};
    double      avg_latency_us{0.0};
    /// 供 TASK-039 换算 QPS：calls / 统计窗口秒数。
    uint64_t    calls_for_qps() const noexcept { return calls; }
};

class RpcMetrics {
public:
    /// 进程级单例（为 TASK-039 全局采集预留）。
    static RpcMetrics& Instance() noexcept;

    /// 记录一次尝试。retries>0 表示该次是第 retries 次重试尝试。
    void Record(const void* method, uint64_t latency_us, bool ok, uint32_t retries);

    /// 快照（avg 就地计算）。
    std::vector<RpcMethodSnapshot> Snapshot() const;

    void Reset();

private:
    RpcMetrics() = default;

    struct Agg {
        uint64_t calls{0};
        uint64_t errors{0};
        uint64_t retries{0};
        uint64_t total_latency_us{0};
    };
    mutable std::mutex mu_;
    std::unordered_map<const void*, Agg> by_method_;
};

}  // namespace mmo::rpc
