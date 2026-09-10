// scripting/lua/src/hot_reload/audit_sink.cpp —— TASK-032 · 阶段 6 的落盘实现。
//
// 幂等设计（重要）
// --------------
//   `docs/script-versions.md` 是**被提交进仓库的制品**。如果每次 Append 都无条件追加一行，
//   那么反复跑验收脚本（验收脚本本身会驱动真实热更）会让这个文件每次都被改写 →
//   工作区永远 dirty、每次 diff 都是噪声，审计日志本身也就失去了「差异即变更」的信号。
//   所以按 `(脚本名, 版本, checksum)` 去重：`checksum` 是源码内容指纹，三者相同即同一次
//   激活事件，直接跳过（不写文件）。构造时读取既有文件恢复去重键集合。

#include "mmo/script/hot_reload/audit_sink.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"

namespace mmo::script {
namespace {

constexpr std::string_view kHeader =
    "# 脚本版本审计日志（Script Version Audit Log）\n"
    "\n"
    "> TASK-032 · Lua Hot Reload —— 每次**成功激活**（含人工回滚与自动回滚）追加一行。\n"
    "> 由 `HotReloader` 阶段 6（Commit）经 `MarkdownAuditSink` 写出；按\n"
    "> `(脚本名, 版本, checksum)` 去重，因此**重复运行同一批验收不会改动本文件**。\n"
    "> 运行期持久化必须经 DataService（§13），本文件是开发期审计视图。\n"
    "\n"
    "| 脚本名 | 版本 | checksum | 激活时间(ms) | 操作者 |\n"
    "|---|---|---|---|---|\n";

std::string_view Trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// 解析 `| a | b | c |` 形式的表格行，返回各单元格（已 trim）。
std::vector<std::string_view> SplitRow(std::string_view line) {
    std::vector<std::string_view> cells;
    if (line.empty() || line.front() != '|') {
        return cells;
    }
    std::size_t pos = 1;
    while (pos <= line.size()) {
        const std::size_t bar = line.find('|', pos);
        if (bar == std::string_view::npos) {
            break;
        }
        cells.push_back(Trim(line.substr(pos, bar - pos)));
        pos = bar + 1;
    }
    return cells;
}

bool ParseU64(std::string_view text, std::uint64_t* out) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10U + static_cast<std::uint64_t>(c - '0');
    }
    *out = value;
    return true;
}

bool ParseI64(std::string_view text, std::int64_t* out) noexcept {
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    std::uint64_t magnitude = 0;
    if (!ParseU64(text, &magnitude)) {
        return false;
    }
    *out = negative ? -static_cast<std::int64_t>(magnitude) : static_cast<std::int64_t>(magnitude);
    return true;
}

}  // namespace

std::string ShortChecksum(std::string_view checksum) {
    return std::string(checksum.substr(0, std::min<std::size_t>(8, checksum.size())));
}

MarkdownAuditSink::MarkdownAuditSink(std::string path) : path_(std::move(path)) {
    (void)LoadExisting();  // 文件不存在/不可解析不是错误：首次运行就是「还没有记录」。
}

bool MarkdownAuditSink::SameKey(const Record& record, std::string_view name,
                                const ScriptVersion& version) noexcept {
    return record.script_name == name && record.version.version == version.version &&
           record.version.checksum == version.checksum;
}

core::Result<void> MarkdownAuditSink::LoadExisting() {
    loaded_ = true;
    std::ifstream in(path_);
    if (!in) {
        return core::Result<void>::Ok();  // 首次运行：文件尚不存在
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string_view> cells = SplitRow(line);
        if (cells.size() < 5) {
            continue;  // 标题 / 分隔行 / 说明行
        }
        std::uint64_t version = 0;
        std::int64_t at_ms = 0;
        if (cells[0] == "脚本名" || cells[0].empty()) {
            continue;
        }
        if (!ParseU64(cells[1], &version) || !ParseI64(cells[3], &at_ms)) {
            continue;  // 分隔行 `|---|---|`
        }
        Record record;
        record.script_name.assign(cells[0]);
        record.version.version = static_cast<std::uint32_t>(version);
        record.version.checksum.assign(cells[2]);
        record.version.activated_at_ms = at_ms;
        record.version.activated_by.assign(cells[4]);
        records_.push_back(std::move(record));
    }
    return core::Result<void>::Ok();
}

core::Result<void> MarkdownAuditSink::Rewrite() const {
    std::ofstream out(path_, std::ios::trunc);
    if (!out) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "cannot write audit log: " + path_, core::domain::kLua));
    }
    out << kHeader;
    for (const Record& record : records_) {
        out << "| " << record.script_name << " | " << record.version.version << " | "
            << record.version.checksum << " | " << record.version.activated_at_ms << " | "
            << record.version.activated_by << " |\n";
    }
    if (!out) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "audit log write failed: " + path_,
            core::domain::kLua));
    }
    return core::Result<void>::Ok();
}

core::Result<void> MarkdownAuditSink::Append(const ScriptVersion& version,
                                            std::string_view script_name) {
    if (!loaded_) {
        const core::Result<void> loaded = LoadExisting();
        if (!loaded) {
            return loaded;
        }
    }
    for (const Record& record : records_) {
        if (SameKey(record, script_name, version)) {
            return core::Result<void>::Ok();  // 幂等：同一次激活事件不重复记录
        }
    }
    Record record;
    record.script_name.assign(script_name);
    record.version = version;
    records_.push_back(std::move(record));

    const core::Result<void> written = Rewrite();
    if (!written) {
        records_.pop_back();  // 写失败则回滚内存状态，保持「内存 == 磁盘」
        return written;
    }
    ++written_;
    return core::Result<void>::Ok();
}

}  // namespace mmo::script
