#pragma once

/// TASK-032 · 审计 sink —— 热更版本记录的落点（§8 阶段 6 / §13 Persistence）。
///
/// 为什么把「记录」与「写盘」分开
/// ----------------------------
///   阶段 6 Commit 必须记录 `ScriptVersion`，但 Activate 位于 **Tick 安全点**且必须
///   < 100us（§22）。文件 IO 一旦混进安全点，既违反 §11「External IO 必须异步化，
///   禁止出现在 Tick 内」，也必然顶破停顿预算。因此本模块把两件事拆开：
///     - `HotReloader` 在安全点内只做**内存入队**（无 IO，纳秒级）；
///     - 宿主在 Tick 之外调用 `HotReloader::DrainAudit()`，由 `IAuditSink` 落盘。
///
/// 持久化口径（§13）
/// ----------------
///   §13 要求「持久化只能经 DataService」。本节提供的 `MarkdownAuditSink` 是**开发期
///   审计日志**（仓库内 `docs/script-versions.md`，人可读、可 code review），
///   不替代运行时持久化。运行期落库需由宿主实现 `IAuditSink` 并经 DataService 写入
///   —— TASK-032 的依赖集（003/013/031）**不含** DataService（TASK-026），
///   故本任务只提供抽象 + 文件实现，落库实现留给后续任务（详见 docs/HOTRELOAD.md §7）。

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "mmo/core/error/result.h"

#include "mmo/script/hot_reload/hot_reloader.h"

namespace mmo::script {

/// 审计记录落点。实现必须**线程安全**（`DrainAudit` 允许在任意非 Tick 线程调用）。
class IAuditSink {
public:
    virtual ~IAuditSink() = default;

    /// 写出一条激活记录。`script_name` 为脚本名（`ScriptVersion` 只带 id，人类审计需要名字）。
    /// 返回错误时 `HotReloader` 会把该记录**重新入队**，不丢审计（宁可重试也不静默丢弃）。
    virtual core::Result<void> Append(const ScriptVersion& version,
                                      std::string_view script_name) = 0;
};

/// 把审计记录写成 Markdown 表格（默认 `docs/script-versions.md`）。
///
/// **幂等**：按 `(脚本名, 版本, checksum)` 三元组去重。重复运行同一批热更（例如反复跑
/// 验收脚本）不会重复追加、也不会改动文件 —— 这一点很重要：审计日志是**被提交进仓库的
/// 制品**，若每次运行都追加带时间戳的新行，工作区会永远处于 dirty 状态。
/// 构造时会读取既有文件以恢复去重键集合。
class MarkdownAuditSink final : public IAuditSink {
public:
    explicit MarkdownAuditSink(std::string path);

    core::Result<void> Append(const ScriptVersion& version, std::string_view script_name) override;

    /// 已知记录数（含构造时从文件恢复的）。
    std::size_t Records() const noexcept { return records_.size(); }
    /// 本次进程真正**新写入**的记录数。
    std::size_t Written() const noexcept { return written_; }
    const std::string& Path() const noexcept { return path_; }

private:
    struct Record {
        std::string script_name;
        ScriptVersion version;
    };

    core::Result<void> LoadExisting();
    core::Result<void> Rewrite() const;
    static bool SameKey(const Record& a, std::string_view name, const ScriptVersion& v) noexcept;

    std::string path_;
    std::vector<Record> records_;
    std::size_t written_{0};
    bool loaded_{false};
};

/// 为阅读方便渲染成一个 checksum 短前缀（前 8 位）。
std::string ShortChecksum(std::string_view checksum);

}  // namespace mmo::script
