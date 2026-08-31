#pragma once

/// TASK-016 · 持久化抽象层（§11 外部 IO / §13 Persistence / §21 Forbidden）。
///
/// 红线：Role 模块**禁止**直接访问 MySQL / Redis / 网络，也禁止在 Tick 内同步等待存档。
/// 所有落盘只能经本接口**异步投递**到 Persistence 线程（§9 线程模型）。
///
/// TASK-026（DataService）会提供真实实现；本任务只定义契约 + 一个内存实现供测试/bench 使用，
/// 从而让 Role 模块在 TASK-026 落地前即可独立编译、测试与验收（§27.3 禁止循环依赖）。

#include <cstddef>
#include <cstdint>
#include <vector>

#include "mmo/core/error/result.h"
#include "mmo/game/role/character.h"

namespace mmo::game::role {

/// 异步存档接口（§7 / §15.8）。
class IPersistenceAdapter {
public:
    virtual ~IPersistenceAdapter() = default;

    /// 把角色快照投递到异步写队列。**禁止阻塞**：只做序列化 + 入队，不等待落盘。
    /// 失败（队列满 / 后端不可用）返回 Fail，由调用方转入重试队列（§19）。
    virtual core::Result<void> EnqueueSave(const Character& c) = 0;

    /// 待写队列长度（观测用）。
    virtual std::size_t PendingCount() const noexcept = 0;
};

/// 内存实现（测试 / benchmark）：只计数不落盘，失败次数可注入，用于演练重试队列（§19）。
class InMemoryPersistenceAdapter final : public IPersistenceAdapter {
public:
    core::Result<void> EnqueueSave(const Character& c) override;
    std::size_t PendingCount() const noexcept override { return pending_.size(); }

    // ---- 观测 / 演练辅助 ----
    std::size_t TotalEnqueued() const noexcept { return total_enqueued_; }
    std::size_t FailCount() const noexcept { return fail_count_; }
    /// 注入接下来 n 次入队失败（模拟 DataService 不可用）。
    void InjectFailures(std::size_t n) noexcept { fail_remaining_ = n; }
    /// 模拟后台线程已消费队列。
    void Drain() noexcept { pending_.clear(); }
    const std::vector<CharacterId>& PendingIds() const noexcept { return pending_; }

private:
    std::vector<CharacterId> pending_;   // 只留 key，避免放大 bench 的内存测量
    std::size_t total_enqueued_{0};
    std::size_t fail_count_{0};
    std::size_t fail_remaining_{0};
};

}  // namespace mmo::game::role
