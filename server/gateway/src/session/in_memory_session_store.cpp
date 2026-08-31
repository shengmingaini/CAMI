// server/gateway/src/session/in_memory_session_store.cpp — TASK-009 §15.2
//
// 无锁单机存储：slot + generation 直接寻址，仅 player 反查需要哈希索引。
// 线程模型（§9）：由 Gateway NetworkThread 独占写，跨线程读走 Snapshot()。

#include "mmo/gateway/session/session_store.h"

namespace mmo::gateway {
namespace {

using core::ErrorCode;

// 哈希节点实际占用 = 节点结构 + 分配器开销（malloc header 通常 16B）。
// 用于 AllocatedBytes 的内存核算（bench per_session_bytes 指标）。
constexpr std::size_t kNodeOverheadBytes = 16;

core::Error MakeError(ErrorCode code, const char* message) {
    // domain 取受控集合（§27：只允许既有的 7 个域）；会话属网关网络层语义 → kNet。
    return core::Error{code, message, core::domain::kNet};
}

}  // namespace

// 刻意不预 reserve(max_sessions)：默认 50000 会一次性吃掉 ~3MB，
// 而 bench 只建 10K 会话。让 vector 按 2x 自然增长更省，且 AllocatedBytes
// 统计的是已用槽位（size），增长冗余不计入每会话指标。
InMemorySessionStore::InMemorySessionStore(Options options) : options_(options) {}

core::Result<SessionId> InMemorySessionStore::Allocate(const Session& initial) {
    if (size_ >= options_.max_sessions) {
        return core::Result<SessionId>::Fail(
            MakeError(ErrorCode::BUSY, "session store full"));  // §15.6 禁止无界增长
    }

    std::uint32_t slot = 0;
    if (!free_slots_.empty()) {
        slot = free_slots_.back();
        free_slots_.pop_back();
    } else {
        slot = static_cast<std::uint32_t>(slots_.size());
        slots_.emplace_back();
        // generation 从 1 起：slot=0/gen=0 会拼出 id=0，与 kInvalidSessionId 冲突。
        generations_.push_back(1);
    }

    Session stored = initial;
    stored.session_id = MakeSessionId(slot, generations_[slot]);
    slots_[slot] = stored;
    ++size_;

    if (stored.player_id != kInvalidPlayerId) {
        index_player_[stored.player_id] = slot;
    }
    return core::Result<SessionId>::Ok(stored.session_id);
}

core::Result<void> InMemorySessionStore::Put(const Session& session) {
    if (!SlotValid(session.session_id)) {
        return core::Result<void>::Fail(
            MakeError(ErrorCode::INVALID_ARGUMENT, "session slot invalid"));
    }
    const std::uint32_t slot = SessionSlot(session.session_id);
    const PlayerId prev_player = slots_[slot].player_id;
    slots_[slot] = session;

    // 维护 player 反查索引：换绑玩家时清理旧键，避免脏索引。
    if (prev_player != session.player_id) {
        if (prev_player != kInvalidPlayerId) {
            index_player_.erase(prev_player);
        }
        if (session.player_id != kInvalidPlayerId) {
            index_player_[session.player_id] = slot;
        }
    }
    return core::Result<void>::Ok();
}

core::Result<std::optional<Session>> InMemorySessionStore::Get(SessionId id) {
    if (!SlotValid(id)) {
        return core::Result<std::optional<Session>>::Ok(std::nullopt);
    }
    return core::Result<std::optional<Session>>::Ok(
        std::optional<Session>{slots_[SessionSlot(id)]});
}

core::Result<std::optional<Session>> InMemorySessionStore::FindByPlayer(PlayerId player) {
    const auto it = index_player_.find(player);
    if (it == index_player_.end()) {
        return core::Result<std::optional<Session>>::Ok(std::nullopt);
    }
    const Session& s = slots_[it->second];
    if (s.player_id != player || s.state == SessionState::Closed) {
        return core::Result<std::optional<Session>>::Ok(std::nullopt);
    }
    return core::Result<std::optional<Session>>::Ok(std::optional<Session>{s});
}

core::Result<void> InMemorySessionStore::Remove(SessionId id) {
    if (!SlotValid(id)) {
        return core::Result<void>::Ok();  // 幂等（§15 重复清理不应报错）
    }
    const std::uint32_t slot = SessionSlot(id);
    if (slots_[slot].player_id != kInvalidPlayerId) {
        index_player_.erase(slots_[slot].player_id);
    }
    slots_[slot] = Session{};                 // 归还槽位内容
    slots_[slot].state = SessionState::Closed;  // 标记空闲（ForEach 跳过）
    ++generations_[slot];                     // 防 ABA：旧 SessionId 立即失效
    free_slots_.push_back(slot);
    --size_;
    return core::Result<void>::Ok();
}

bool InMemorySessionStore::SlotValid(SessionId id) const noexcept {
    const std::uint32_t slot = SessionSlot(id);
    if (slot >= slots_.size()) {
        return false;
    }
    if (generations_[slot] != SessionGeneration(id)) {
        return false;  // 槽位已复用，旧 id 作废（防回放）
    }
    return slots_[slot].state != SessionState::Closed;
}

std::size_t InMemorySessionStore::AllocatedBytes() const noexcept {
    using PlayerNode = decltype(index_player_)::value_type;
    // 统计**实际持有量**：已用槽位 + 哈希索引（桶数组 + 节点）。
    // 不含 vector / 哈希表扩容时的临时内存，保证指标稳定可复现。
    return slots_.size() * sizeof(Session)
         + generations_.size() * sizeof(std::uint32_t)
         + index_player_.bucket_count() * sizeof(void*)
         + index_player_.size() * (sizeof(PlayerNode) + kNodeOverheadBytes);
}

std::vector<Session> InMemorySessionStore::Snapshot() const {
    std::vector<Session> out;
    out.reserve(size_);
    ForEach([&out](const Session& s) { out.push_back(s); return true; });
    return out;
}

}  // namespace mmo::gateway
