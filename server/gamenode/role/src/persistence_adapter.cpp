/// TASK-016 · 内存持久化适配器实现（测试 / benchmark 用）。
///
/// 真实实现由 TASK-026（DataService）提供；本实现只做入队与计数，
/// 用于在 TASK-026 落地前独立验收 Role 模块的存档路径（§19 / §27.3）。

#include "mmo/game/role/persistence_adapter.h"

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::game::role {

core::Result<void> InMemoryPersistenceAdapter::EnqueueSave(const Character& c) {
    if (fail_remaining_ > 0) {
        --fail_remaining_;
        ++fail_count_;
        // 模拟 DataService 不可用：不入队、不阻塞（§19）。
        // 用 BUSY（ErrorCode 无 UNAVAILABLE），它属于 IsRetryable 集合，语义为「稍后重试」。
        return core::Result<void>::Fail(core::Error(core::ErrorCode::BUSY,
                                                    "persistence backend unavailable",
                                                    core::domain::kCore));
    }
    (void)c;  // 只留 key：避免放大 bench 的 mem_bytes_per_character 测量
    pending_.push_back(c.id);
    ++total_enqueued_;
    return core::Result<void>::Ok();
}

}  // namespace mmo::game::role
