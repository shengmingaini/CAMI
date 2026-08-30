// engine/core/src/log/log_context.cpp — TASK-002 thread_local 日志上下文
//
// 设计要点（§9 Thread Model）：
//   - 上下文存 thread_local，读写无锁、无原子，读取成本≈一次 TLS 访问；
//   - 跨线程不自动延续，必须显式拷贝 + WithContext 套用；
//   - ScopedLogContext 析构即恢复，保证异常安全与嵌套正确。

#include "mmo/core/log/log_context.h"

namespace mmo::core {
namespace {

thread_local LogContext g_tls_context{};

/// 逐字段覆盖：仅覆盖「非哨兵值」字段，未设置的维度继承外层上下文。
void ApplyPatch(const LogContext& patch, LogContext& target) noexcept {
    if (patch.trace_id != kInvalidTraceId) {
        target.trace_id = patch.trace_id;
    }
    if (patch.request_id != kInvalidRequestId) {
        target.request_id = patch.request_id;
    }
    if (patch.player_id != kInvalidPlayerId) {
        target.player_id = patch.player_id;
    }
    if (patch.scene_id != kInvalidSceneId) {
        target.scene_id = patch.scene_id;
    }
    if (!patch.module.empty()) {
        target.module = patch.module;
    }
}

}  // namespace

const LogContext& CurrentLogContext() noexcept {
    return g_tls_context;
}

ScopedLogContext::ScopedLogContext(const LogContext& patch) noexcept : saved_(g_tls_context) {
    ApplyPatch(patch, g_tls_context);
}

ScopedLogContext::ScopedLogContext(const LogContext& full, ReplaceAllTag) noexcept
    : saved_(g_tls_context) {
    g_tls_context = full;
}

ScopedLogContext::~ScopedLogContext() {
    g_tls_context = saved_;
}

}  // namespace mmo::core
