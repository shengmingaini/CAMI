// echo_service.cpp — 回声服务实现（TASK-006 测试/bench 专用）
#include "echo_service.h"

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace mmo::rpc::testing {

namespace {

struct HintState {
    uint32_t calls{0};
    std::unordered_set<std::string> idem_keys;
};

std::mutex g_mu;
std::unordered_map<int64_t, HintState> g_hints;

HintState& State(int64_t hint) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_hints[hint];
}

}  // namespace

grpc::Status EchoServiceImpl::Echo(grpc::ServerContext* ctx, const EchoRequest* req,
                                   EchoResponse* resp) {
    auto& st = State(req->trace_hint());
    {
        std::lock_guard<std::mutex> lk(g_mu);
        st.calls += 1;
        for (const auto& [k, v] : ctx->client_metadata()) {
            if (k == "idempotency_key") st.idem_keys.emplace(v.begin(), v.end());
        }
    }

    if (req->fail_unavailable() > 0 && st.calls <= req->fail_unavailable()) {
        return grpc::Status{grpc::StatusCode::UNAVAILABLE, "injected unavailable"};
    }
    if (req->fail_deadline() > 0 && st.calls <= req->fail_deadline()) {
        return grpc::Status{grpc::StatusCode::DEADLINE_EXCEEDED, "injected deadline"};
    }
    if (req->delay_ms() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{req->delay_ms()});
    }

    resp->set_message(req->message());
    resp->set_server_recv_unix_ms(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    for (const auto& [k, v] : ctx->client_metadata()) {
        if (k == "trace_id") resp->set_trace_id(v.begin(), v.size());
    }
    if (req->payload_bytes() > 0) {
        static const std::string fill(4096, 'x');
        while (resp->message().size() < req->payload_bytes()) {
            resp->mutable_message()->append(fill);
        }
    }
    return grpc::Status::OK;
}

uint32_t EchoServiceImpl::CallsFor(int64_t hint) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_hints.find(hint);
    return it == g_hints.end() ? 0u : it->second.calls;
}

size_t EchoServiceImpl::KeysFor(int64_t hint) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_hints.find(hint);
    return it == g_hints.end() ? 0u : it->second.idem_keys.size();
}

void EchoServiceImpl::ResetCounts() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hints.clear();
}

}  // namespace mmo::rpc::testing
