/// TASK-034 · NetClient 实现（连接 / 重连 / 心跳 / 超时状态机，单读线程模型）。

#include "mmo/client/net_client.h"

#include "mmo/protocol/codec/envelope_validator.h"
#include "mmo/protocol/codec/flatbuf_codec.h"
#include "mmo/protocol/codec/protobuf_codec.h"

#include "mmo/client/stats.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace mmo { namespace client {

NetClient::NetClient(NetConfig cfg)
    : cfg_(std::move(cfg)) {
    if (cfg_.codec == 1) codec_ = std::make_unique<protocol::ProtobufCodec>();
    else                codec_ = std::make_unique<protocol::FlatbufCodec>();
}

NetClient::~NetClient() {
    Disconnect();
}

core::Result<void> NetClient::ConnectOnce() {
    auto r = link_.Connect(cfg_.addr, cfg_.connect_timeout_ms);
    if (!r.HasValue()) return core::Result<void>::Fail(r.Err());
    return core::Result<void>::Ok();
}

void NetClient::StopRecvThread() noexcept {
    if (recv_running_.exchange(false)) {
        // 让阻塞中的 Recv 通过关闭链路返回；recv 线程随后自然退出。
        link_.Close();
        if (recv_thread_.joinable()) recv_thread_.join();
    }
}

void NetClient::Disconnect() noexcept {
    StopRecvThread();
    link_.Close();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_resp_.clear();
        // 唤醒任何等待方，让其以失败返回
    }
    resp_cv_.notify_all();
    state_.store(NetState::Disconnected);
}

core::Result<void> NetClient::Connect() {
    Disconnect();  // 清理任何旧状态
    state_.store(NetState::Connecting);
    auto r = ConnectOnce();
    if (!r.HasValue()) {
        if (cfg_.reconnect_enabled) {
            auto rc = Reconnect();
            if (!rc.HasValue()) {
                state_.store(NetState::Disconnected);
                return core::Result<void>::Fail(rc.Err());
            }
            return core::Result<void>::Ok();
        }
        state_.store(NetState::Disconnected);
        return core::Result<void>::Fail(r.Err());
    }
    recv_running_.store(true);
    recv_thread_ = std::thread(&NetClient::RecvLoop, this);
    state_.store(NetState::Connected);
    return core::Result<void>::Ok();
}

core::Result<void> NetClient::Reconnect() {
    state_.store(NetState::Reconnecting);
    StopRecvThread();
    link_.Close();
    const int max_attempts = std::max(1, cfg_.max_reconnect_attempts);
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.reconnect_base_delay_ms * static_cast<std::uint32_t>(attempt)));
        auto r = ConnectOnce();
        if (r.HasValue()) {
            ++metrics_.reconnects;
            recv_running_.store(true);
            recv_thread_ = std::thread(&NetClient::RecvLoop, this);
            state_.store(NetState::Connected);
            return core::Result<void>::Ok();
        }
        ++metrics_.errors;
    }
    state_.store(NetState::Disconnected);
    return core::Result<void>::Fail(core::Error(
        core::ErrorCode::UNAUTHORIZED, "重连失败：达到最大尝试次数", core::domain::kNet));
}

void NetClient::RecvLoop() {
    while (recv_running_.load()) {
        auto frame = link_.Recv(cfg_.recv_timeout_ms);
        if (!frame.HasValue()) {
            // 超时（无数据）属正常轮询；其它错误视为断线。
            if (frame.Err().Code() == core::ErrorCode::TIMEOUT) continue;
            break;  // 断线
        }
        OnFrame(frame.Value());
    }
    // 断线处理：尝试重连一次；失败则进入 Disconnected 并唤醒等待方。
    bool should_reconnect = cfg_.reconnect_enabled && state_.load() != NetState::Disconnected;
    if (should_reconnect) {
        recv_running_.store(false);
        // 当前线程即将退出：先把自己从 recv_thread_ 解绑（detach）。
        // 否则 Reconnect 内对新线程的 move-assignment 会销毁「仍在运行」的旧线程
        // 句柄，而 std::thread 在 *this 仍 joinable 时直接 std::terminate
        // （表现为 "terminate called without an active exception"）。
        recv_thread_.detach();
        auto rc = Reconnect();
        if (rc.HasValue()) return;  // Reconnect 已重启 recv 线程
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_resp_.clear();
    }
    resp_cv_.notify_all();
    state_.store(NetState::Disconnected);
}

void NetClient::OnFrame(const std::vector<std::uint8_t>& frame) {
    auto dec = codec_->Decode(std::string_view(
        reinterpret_cast<const char*>(frame.data()), frame.size()));
    if (!dec.HasValue()) { ++metrics_.errors; return; }
    const protocol::EnvelopeView& v = dec.Value().view();
    auto valid = protocol::EnvelopeValidator::Validate(v, kProtoVersion);
    if (!valid.HasValue()) { ++metrics_.errors; return; }

    ++metrics_.received;
    if (v.message_type == protocol::EnvelopeMessageType::Response) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_resp_[v.request_id] = std::vector<std::uint8_t>(
                dec.Value().data(), dec.Value().data() + dec.Value().size());
        }
        resp_cv_.notify_all();  // 唤醒等待中的 Request
    } else if (v.message_type == protocol::EnvelopeMessageType::Event) {
        {
        std::lock_guard<std::mutex> lk(mtx_);
        event_queue_.emplace_back(v.payload.data(),
                                  v.payload.data() + v.payload.size());
        }
        resp_cv_.notify_all();
    }
    // 其它类型（Command/Query/Heartbeat 响应等）此处忽略。
}

core::Result<std::vector<std::uint8_t>> NetClient::WaitResponse(
    std::uint64_t request_id, double sent_at_ms, std::uint32_t timeout_ms) {
    const std::uint32_t to = timeout_ms > 0 ? timeout_ms : cfg_.request_timeout_ms;
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(to);
    std::unique_lock<std::mutex> lk(mtx_);
    while (true) {
        auto it = pending_resp_.find(request_id);
        if (it != pending_resp_.end()) {
            std::vector<std::uint8_t> payload = std::move(it->second);
            pending_resp_.erase(it);
            const double now = NowMs();
            rtt_samples_.push_back(std::max(0.0, now - sent_at_ms));
            lk.unlock();
            if (rtt_samples_.size() >= 32) {
                std::vector<double> sorted = rtt_samples_;
                metrics_.rtt_ms_p95 = Percentile(sorted, 95.0);
            }
            return core::Result<std::vector<std::uint8_t>>::Ok(std::move(payload));
        }
        if (state_.load() != NetState::Connected) break;
        if (resp_cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
    }
    return core::Result<std::vector<std::uint8_t>>::Fail(core::Error(
        core::ErrorCode::TIMEOUT, "等待 Response 超时", core::domain::kNet));
}

protocol::EnvelopeView NetClient::MakeEnvelope(protocol::EnvelopeMessageType t,
                                              std::uint64_t request_id,
                                              std::string_view payload) {
    protocol::EnvelopeView v;
    v.message_id  = next_msg_id_++;
    v.request_id  = request_id;
    v.message_type = t;
    v.version     = kProtoVersion;
    v.source      = "client";
    v.timestamp_ms = 0;
    v.payload     = payload;
    return v;
}

core::Result<std::vector<std::uint8_t>> NetClient::Request(
    std::string_view payload, std::uint32_t timeout_ms) {
    if (state_.load() != NetState::Connected)
        return core::Result<std::vector<std::uint8_t>>::Fail(core::Error(
            core::ErrorCode::UNAUTHORIZED, "未连接", core::domain::kNet));

    const std::uint64_t rid = next_msg_id_;
    auto env = MakeEnvelope(protocol::EnvelopeMessageType::Command, rid, payload);
    auto enc = codec_->Encode(env);
    if (!enc.HasValue())
        return core::Result<std::vector<std::uint8_t>>::Fail(enc.Err());

    const double t0 = NowMs();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_resp_.erase(rid);
    }
    auto send_r = link_.Send(std::string_view(
        reinterpret_cast<const char*>(enc.Value().data()), enc.Value().size()));
    if (!send_r.HasValue()) {
        ++metrics_.errors;
        if (cfg_.reconnect_enabled) {
            auto rc = Reconnect();
            if (rc.HasValue()) {
                // 重连后由调用方决定是否重试；这里返回失败，让上层感知断线。
                return core::Result<std::vector<std::uint8_t>>::Fail(send_r.Err());
            }
        }
        return core::Result<std::vector<std::uint8_t>>::Fail(send_r.Err());
    }
    ++metrics_.sent;

    auto resp = WaitResponse(rid, t0, timeout_ms);
    if (!resp.HasValue()) {
        ++metrics_.errors;
        return core::Result<std::vector<std::uint8_t>>::Fail(resp.Err());
    }
    return core::Result<std::vector<std::uint8_t>>::Ok(resp.Value());
}

core::Result<void> NetClient::Send(std::string_view payload,
                                   protocol::EnvelopeMessageType t) {
    if (state_.load() != NetState::Connected)
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::UNAUTHORIZED, "未连接", core::domain::kNet));
    const std::uint64_t rid = next_msg_id_;
    auto env = MakeEnvelope(t, rid, payload);
    auto enc = codec_->Encode(env);
    if (!enc.HasValue()) return core::Result<void>::Fail(enc.Err());
    auto r = link_.Send(std::string_view(
        reinterpret_cast<const char*>(enc.Value().data()), enc.Value().size()));
    if (!r.HasValue()) { ++metrics_.errors; return core::Result<void>::Fail(r.Err()); }
    ++metrics_.sent;
    return core::Result<void>::Ok();
}

core::Result<void> NetClient::SendHeartbeat() {
    auto r = Send("", protocol::EnvelopeMessageType::Heartbeat);
    if (!r.HasValue()) return core::Result<void>::Fail(r.Err());
    return core::Result<void>::Ok();
}

bool NetClient::TryRecvEvent(std::vector<std::uint8_t>& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (event_queue_.empty()) return false;
    out = std::move(event_queue_.front());
    event_queue_.pop_front();
    return true;
}

}}  // namespace mmo::client
