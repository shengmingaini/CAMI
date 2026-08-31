// server/gateway/include/mmo/gateway/session/session_store.h — TASK-009 §15.2
//
// ISessionStore：Session 存储抽象，为 TASK-027（Redis 适配器）预留同接口替换。
// InMemorySessionStore：第一版单机实现。
//
// 设计取舍（为什么不用 unordered_map<SessionId, Session>）：
//   SessionId 本身编码了 slot + generation（见 session.h），因此 Get(SessionId)
//   是 O(1) 直接寻址，**不需要 id → 槽位的哈希索引**。省掉这张表后每会话内存
//   从 ~180B 降到 ~120B，对 §22「单会话 < 256B」与 50K 规模都留足余量。
//
// 线程模型（§9）：Store 由 Gateway 的 NetworkThread 独占写，**无任何锁**
// （§21 禁止用全局锁保护 Session 表）。跨线程查询走 Snapshot() 不可变快照。

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

// ---------------------------------------------------------------------------
// ISessionStore（§7 Public Interface，签名与任务书一致，冻结后不可破坏性变更）
// ---------------------------------------------------------------------------

class ISessionStore {
public:
    virtual ~ISessionStore() = default;

    /// 写入 / 更新一个已分配槽位的会话。session_id 无效（槽位未分配或 generation
    /// 不匹配）时返回 INVALID_ARGUMENT——新增会话请先 Allocate()。
    virtual core::Result<void> Put(const Session& session) = 0;

    /// 按 SessionId 读取；不存在或 generation 失效返回 nullopt（不是错误）。
    virtual core::Result<std::optional<Session>> Get(SessionId id) = 0;

    /// 按 PlayerId 反查（登录互斥 / 防双开）；不存在返回 nullopt。
    virtual core::Result<std::optional<Session>> FindByPlayer(PlayerId player) = 0;

    /// 移除并回收槽位；幂等（重复 Remove 返回 OK）。
    virtual core::Result<void> Remove(SessionId id) = 0;
};

// ---------------------------------------------------------------------------
// InMemorySessionStore（第一版）
//
// 存储布局：
//   slots_        —— 紧凑 vector<Session>，O(1) 寻址，Tick 全量扫描对 cache 友好
//   generations_  —— 每槽代次，槽位复用后旧 SessionId 立即失效（防 ABA / 回放）
//   free_slots_   —— 空闲槽栈，O(1) 分配与回收
//   index_player_ —— 唯一的哈希索引（PlayerId → slot），仅供 FindByPlayer
// ---------------------------------------------------------------------------

// Options 刻意定义在命名空间级而非类内嵌套：
// GCC 下「嵌套 struct 带 NSDMI + 作默认实参（Options options = {}）」会编译失败
// （TASK-007 EventBus 已踩过同一个坑）。类内用 using 保持调用方写法不变。
struct SessionStoreOptions {
    std::size_t max_sessions{50000};  // §7 Config::max_sessions 默认值
};

class InMemorySessionStore final : public ISessionStore {
public:
    using Options = SessionStoreOptions;

    explicit InMemorySessionStore(Options options = Options{});

    // ---- ISessionStore ----
    core::Result<void> Put(const Session& session) override;
    core::Result<std::optional<Session>> Get(SessionId id) override;
    core::Result<std::optional<Session>> FindByPlayer(PlayerId player) override;
    core::Result<void> Remove(SessionId id) override;

    // ---- 扩展能力（非接口，供 SessionManager / bench / 测试使用）----

    /// 分配一个新槽位并写入初始状态，返回带 generation 的 SessionId。
    /// 容量达 max_sessions 时返回 BUSY（§15.6 禁止无界增长）。
    core::Result<SessionId> Allocate(const Session& initial);

    /// 当前存活会话数（不含已回收槽位）。
    std::size_t Size() const noexcept { return size_; }
    std::size_t Capacity() const noexcept { return options_.max_sessions; }

    /// 实际持有内存（字节）。供 §22 per_session_bytes 指标使用。
    /// 刻意统计**持有量**而非累计分配量：vector / 哈希表扩容时的临时内存
    /// 不计入，指标稳定可复现。
    std::size_t AllocatedBytes() const noexcept;

    /// 不可变快照（跨线程查询 / 测试断言用）。热路径禁止调用（会分配）。
    std::vector<Session> Snapshot() const;

    /// 只读遍历（Tick 扫描用，零分配，跳过空闲槽）。
    /// callback 返回 false 可提前终止。
    template <typename Fn>
    void ForEach(Fn&& callback) const {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state == SessionState::Closed) {
                continue;  // 空闲槽
            }
            if (!callback(slots_[i])) {
                return;
            }
        }
    }

    /// 可写遍历（Tick 超时处理用，零分配）。callback 可直接修改 Session。
    template <typename Fn>
    void ForEachMutable(Fn&& callback) {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].state == SessionState::Closed) {
                continue;
            }
            if (!callback(slots_[i])) {
                return;
            }
        }
    }

private:
    bool SlotValid(SessionId id) const noexcept;

    Options                                    options_;
    std::vector<Session>                       slots_;
    std::vector<std::uint32_t>                 generations_;
    std::vector<std::uint32_t>                 free_slots_;
    std::unordered_map<PlayerId, std::uint32_t> index_player_;
    std::size_t                                size_{0};
};

}  // namespace mmo::gateway
