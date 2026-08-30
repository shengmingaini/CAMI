// rpc_metrics.cpp — 客户端 per-method 指标实现（TASK-006 §15.8）
#include "mmo/rpc/rpc_metrics.h"

namespace mmo::rpc {

RpcMetrics& RpcMetrics::Instance() noexcept {
    static RpcMetrics inst;
    return inst;
}

void RpcMetrics::Record(const void* method, uint64_t latency_us, bool ok, uint32_t retries) {
    std::lock_guard<std::mutex> lk(mu_);
    auto& a = by_method_[method];
    a.calls += 1;
    if (!ok) a.errors += 1;
    a.retries += retries;
    a.total_latency_us += latency_us;
}

std::vector<RpcMethodSnapshot> RpcMetrics::Snapshot() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<RpcMethodSnapshot> out;
    out.reserve(by_method_.size());
    for (const auto& [k, a] : by_method_) {
        RpcMethodSnapshot s;
        s.method = k;
        s.calls = a.calls;
        s.errors = a.errors;
        s.retries = a.retries;
        s.total_latency_us = a.total_latency_us;
        s.avg_latency_us = a.calls ? static_cast<double>(a.total_latency_us) /
                                         static_cast<double>(a.calls) : 0.0;
        out.push_back(s);
    }
    return out;
}

void RpcMetrics::Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    by_method_.clear();
}

}  // namespace mmo::rpc
