// scripting/lua/src/hot_reload/hot_reloader.cpp —— TASK-032 · 六阶段热更流水线。
//
// 线程模型（§9，实现要点）
// ----------------------
//   * `Prepare` / `Validate`（阶段 1-3）在 **Worker 线程**：它们只碰一个**隔离的临时
//     `ScriptContext`**（§15-3「在隔离的临时 VM 中跑，不动生产 VM」），因此完全不与
//     生产 VM 的 OwnerThread 约束冲突。
//   * `Activate` / `Rollback` / `VerifyPass`（阶段 4-5）在 **SimulationThread 的安全点**：
//     它们直接改生产 VM，所以必须同时满足 `InSafePoint()`、`OnOwnerThread()`、
//     `!InScriptExecution()` 三个条件，任一不满足即 `BUSY`。
//   * `mu_` 保护 `entries_` / `audit_queue_`。持锁期间会调用 `ScriptContext`
//     （激活 / 冒烟），但那只是一次同步 Lua 调用，无回调进入本对象，故无死锁风险。
//
// 【踩坑 · 隔离 VM 的释放】`ScriptContext::Create` 每次都会新建一个 `lua_State`
//   （计数 allocator 上限 8MiB，实测常驻 ~14KB），函数末尾必须让其 `unique_ptr` 析构，
//   否则「连续热更不泄漏」（§19）无从谈起。

#include "mmo/script/hot_reload/hot_reloader.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mmo/core/time/clock.h"

#include "mmo/script/hot_reload/audit_sink.h"

namespace mmo::script {
namespace {

// ---------------------------------------------------------------------------
// 小工具（刻意不引入 <cstdio> / <iostream>：本仓库红线禁止裸日志输出）
// ---------------------------------------------------------------------------

/// FNV-1a 64 → 16 位十六进制。非加密内容指纹，见头文件 `ScriptVersion::checksum` 的说明。
std::string ChecksumOf(std::string_view data) noexcept {
    std::uint64_t hash = 0xcbf29ce484222325ULL;  // FNV offset basis
    for (const char ch : data) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 0x00000100000001b3ULL;           // FNV prime
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[static_cast<std::size_t>(hash & 0xFU)];
        hash >>= 4;
    }
    return out;
}

/// 十进制（避免 <cstdio>）。
std::string ToDecimal(std::uint64_t value) {
    if (value == 0) {
        return "0";
    }
    char digits[20];
    int n = 0;
    while (value > 0 && n < 20) {
        digits[n++] = static_cast<char>('0' + static_cast<int>(value % 10U));
        value /= 10U;
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = n - 1; i >= 0; --i) {
        out.push_back(digits[i]);
    }
    return out;
}

bool IsIdentChar(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// 跳过 `open_pos`（指向 `[`）处的长括号；成功返回结束位置（`]` 之后），失败返回 npos。
std::size_t SkipLongBracket(std::string_view src, std::size_t open_pos) noexcept {
    const std::size_t n = src.size();
    if (open_pos >= n || src[open_pos] != '[') {
        return std::string_view::npos;
    }
    std::size_t eq = 0;
    std::size_t k = open_pos + 1;
    while (k < n && src[k] == '=') {
        ++eq;
        ++k;
    }
    if (k >= n || src[k] != '[') {
        return std::string_view::npos;
    }
    std::size_t close = k + 1;
    while (close < n) {
        if (src[close] == ']') {
            std::size_t e = 0;
            std::size_t m = close + 1;
            while (m < n && src[m] == '=') {
                ++e;
                ++m;
            }
            if (e == eq && m < n && src[m] == ']') {
                return m + 1;
            }
        }
        ++close;
    }
    return n;
}

/// 把 Lua 源码里的**注释与字符串**替换成空格，只留下可扫的「代码骨架」。
///
/// 为什么必须先剥离：静态禁用 API 扫描是 §8 阶段 3 的要求，但裸子串扫描会把
/// 注释里的 "load the config" 或字符串里的 "io" 误判成违规 —— **误报比漏报更有害**，
/// 它会让完全合法的脚本被拒绝上线。剥离注释/字符串后只剩标识符，误报基本消除。
/// 这不是完整 Lua 词法器：只处理行注释、长括号注释、短字符串、长字符串四类。
std::string StripLuaTrivia(std::string_view src) {
    std::string out;
    out.reserve(src.size());
    const std::size_t n = src.size();
    std::size_t i = 0;
    while (i < n) {
        const char c = src[i];
        if (c == '-' && i + 1 < n && src[i + 1] == '-') {
            const std::size_t after = SkipLongBracket(src, i + 2);
            if (after != std::string_view::npos) {
                i = after;
            } else {
                std::size_t j = i + 2;
                while (j < n && src[j] != '\n') {
                    ++j;
                }
                i = j;
            }
            out.push_back(' ');
            continue;
        }
        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < n) {
                if (src[j] == '\\') {
                    j += 2;
                    continue;
                }
                if (src[j] == c) {
                    ++j;
                    break;
                }
                if (src[j] == '\n') {
                    break;  // 未闭合：就此收尾，让后续文本照常参与扫描
                }
                ++j;
            }
            i = std::min(j, n);
            out.push_back(' ');
            continue;
        }
        if (c == '[') {
            const std::size_t after = SkipLongBracket(src, i);
            if (after != std::string_view::npos) {
                i = after;
                out.push_back(' ');
                continue;
            }
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

/// 静态禁用名单（与 docs/SANDBOX.md §2 的白名单互补：这里列的是**沙箱已移除**的能力）。
/// 命中不代表「能调用成功」（沙箱里它们是 nil），而代表**脚本作者写错了 API**，
/// 提前拦截可以避免上线后才发现整段逻辑静默失效。
constexpr std::string_view kBannedApis[] = {
    "io",       "os",       "package",  "debug",  "coroutine",
    "require",  "dofile",   "loadfile", "load",   "loadstring",
    "collectgarbage", "warn", "module",
    // 白名单外的高危成员（沙箱已排除，静态扫描补一道）
    "string.dump", "math.randomseed",
};

/// 按「标识符边界」搜索一次；每个 token 只报一次（避免噪声淹没报告）。
void ScanBanned(std::string_view code, std::string_view token, std::vector<std::string>* issues) {
    std::size_t pos = 0;
    while ((pos = code.find(token, pos)) != std::string_view::npos) {
        const bool left_ok = (pos == 0) || !IsIdentChar(code[pos - 1]);
        const std::size_t end = pos + token.size();
        const bool right_ok = (end >= code.size()) || !IsIdentChar(code[end]);
        if (left_ok && right_ok) {
            issues->push_back("sandbox: banned api '" + std::string(token) + "'");
            return;
        }
        pos = end;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ReloadState 名称化
// ---------------------------------------------------------------------------

const char* ToString(ReloadState state) noexcept {
    switch (state) {
        case ReloadState::Idle:            return "Idle";
        case ReloadState::Compiling:       return "Compiling";
        case ReloadState::Validating:      return "Validating";
        case ReloadState::PendingActivate: return "PendingActivate";
        case ReloadState::Activated:       return "Activated";
        case ReloadState::RolledBack:      return "RolledBack";
        case ReloadState::Failed:          return "Failed";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------

HotReloader::HotReloader(ScriptContext& context, Config config)
    : context_(context), config_(config) {}

HotReloader::~HotReloader() = default;

void HotReloader::SetIsolatedPreparer(IsolatedPreparer fn, void* user) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    isolated_preparer_ = fn;
    isolated_user_ = user;
}

core::Result<std::unique_ptr<ScriptContext>> HotReloader::CreateIsolated() const {
    IsolatedPreparer preparer = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        preparer = isolated_preparer_;
        user = isolated_user_;
    }
    // 限额沿用生产 VM：校验必须与线上同口径，否则「隔离 VM 通过、生产 VM 触限」就会漏网。
    auto isolated = ScriptContext::Create(context_.Vm().Limits());
    if (!isolated) {
        return core::Result<std::unique_ptr<ScriptContext>>::Fail(core::Error(
            core::ErrorCode::INTERNAL_ERROR, "cannot create isolated lua vm", core::domain::kLua));
    }
    if (preparer != nullptr) {
        const core::Result<void> prepared = preparer(*isolated.Value(), user);
        if (!prepared) {
            // 装配失败（例如绑定重名）→ 冒烟结果不可信，宁可判内部错误也不要放行。
            return core::Result<std::unique_ptr<ScriptContext>>::Fail(core::Error(
                core::ErrorCode::INTERNAL_ERROR,
                "isolated vm binding setup failed: " + std::string(prepared.Err().Message()),
                core::domain::kLua));
        }
    }
    // `Result::Value()` 的 const 左值重载返回 `const T&`，不能从中移动 `unique_ptr`；
    // 用右值重载 `std::move(result).Value()` 取到 `T&&`。
    return core::Result<std::unique_ptr<ScriptContext>>::Ok(std::move(isolated).Value());
}

// ---------------------------------------------------------------------------
// 内部工具
// ---------------------------------------------------------------------------

HotReloader::Entry* HotReloader::FindEntry(std::string_view name) {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

const HotReloader::Entry* HotReloader::FindEntry(std::string_view name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

bool HotReloader::SmokeCall(ScriptContext& target, ScriptId id) const {
    if (id == kInvalidScriptId || config_.smoke_function.empty()) {
        return true;
    }
    if (!target.IsLoaded(id)) {
        return false;
    }
    const core::Result<void> called = target.Call(id, config_.smoke_function);
    if (called) {
        return true;
    }
    // 脚本没有定义冒烟函数 ⇒ 不是错误：并非所有脚本都需要自检入口。
    return called.Err().Code() == core::ErrorCode::NOT_FOUND;
}

core::Result<ScriptId> HotReloader::SwapIn(const std::string& name, const std::string& source) {
    const auto it = entries_.find(name);
    const bool already_loaded =
        it != entries_.end() && it->second.current_id != kInvalidScriptId &&
        context_.IsLoaded(it->second.current_id);
    if (already_loaded) {
        // 热替换：复用旧 `_ENV`，脚本全局状态保留（§17 / §21）。
        return context_.ReloadInPlace(name, source);
    }
    // 首次装载（含「曾经卸载后又部署」的情形）。
    return context_.Load(name, source);
}

void HotReloader::RecordAudit(const ScriptVersion& version, std::string_view name) {
    // 调用方必须已持有 mu_。
    audit_queue_.emplace_back(version, std::string(name));
}

// ---------------------------------------------------------------------------
// 阶段 1-2：Prepare
// ---------------------------------------------------------------------------

core::Result<ReloadTicket> HotReloader::Prepare(std::string_view name, std::string_view source) {
    if (name.empty()) {
        return core::Result<ReloadTicket>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "empty script name", core::domain::kLua));
    }
    if (source.empty()) {
        return core::Result<ReloadTicket>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "empty script source", core::domain::kLua));
    }

    ReloadTicket ticket;
    ticket.name.assign(name);
    ticket.source.assign(source);
    ticket.checksum = ChecksumOf(source);
    ticket.origin = "<inline>";
    ticket.state = ReloadState::Compiling;

    // ---- 阶段 2 Compile：在**隔离临时 VM** 上编译（§15-3）----
    // 生产 VM 属 SimulationThread，Worker 线程碰不得；临时 VM 的 OwnerThread 就是本线程。
    auto isolated = CreateIsolated();
    if (!isolated) {
        failed_count_.fetch_add(1, std::memory_order_relaxed);
        return core::Result<ReloadTicket>::Fail(isolated.Err());
    }
    const core::Result<ScriptId> compiled = isolated.Value()->Load(ticket.name, ticket.source);
    if (!compiled) {
        // 语法错误 / 顶层执行出错：**生产 VM 从未被触碰**，旧版本完全不受影响（§20 验收 #2）。
        failed_count_.fetch_add(1, std::memory_order_relaxed);
        return core::Result<ReloadTicket>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, compiled.Err().Message(), core::domain::kLua));
    }
    ticket.compiled = true;
    ticket.report.syntax_ok = true;

    {
        std::lock_guard<std::mutex> lock(mu_);
        ticket.ticket_id = next_ticket_id_++;
        Entry& entry = entries_[ticket.name];
        entry.state = ReloadState::Validating;
        entry.pending = ticket;
        entry.has_pending = false;  // 校验通过后才置位
    }
    return core::Result<ReloadTicket>::Ok(std::move(ticket));
}

core::Result<ReloadTicket> HotReloader::PrepareFromFile(std::string_view name,
                                                       std::string_view path) {
    if (!config_.allow_filesystem_read) {
        return core::Result<ReloadTicket>::Fail(core::Error(
            core::ErrorCode::UNAUTHORIZED, "filesystem read disabled", core::domain::kLua));
    }
    // §11 External IO：只在 Worker 线程 / 安全点之外发生，绝不出现在 Tick 内。
    std::ifstream in(std::string(path), std::ios::binary);
    if (!in) {
        // §19「磁盘脚本文件被删：Prepare 返回 NOT_FOUND」。
        return core::Result<ReloadTicket>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script file not found: " + std::string(path),
            core::domain::kLua));
    }
    std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return core::Result<ReloadTicket>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "script file unreadable: " + std::string(path),
            core::domain::kLua));
    }
    core::Result<ReloadTicket> ticket = Prepare(name, source);
    if (!ticket) {
        return ticket;
    }
    // `Result::Value()` 是 **const 引用**：必须拷贝出来再改，否则改的是临时量（静默无效）。
    ReloadTicket prepared = ticket.Value();
    prepared.origin.assign(path);
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = entries_.find(name);
        if (it != entries_.end()) {
            it->second.pending.origin.assign(path);
        }
    }
    return core::Result<ReloadTicket>::Ok(std::move(prepared));
}

// ---------------------------------------------------------------------------
// 阶段 3：Validate
// ---------------------------------------------------------------------------

core::Result<ValidationReport> HotReloader::Validate(const ReloadTicket& ticket) {
    const core::SteadyTime t_begin = core::MonotonicClock::Point();
    ValidationReport report;
    report.syntax_ok = ticket.compiled;
    if (!report.syntax_ok) {
        report.issues.push_back("syntax: script was not compiled");
    }

    // ---- 沙箱静态扫描（剥离注释/字符串后按标识符边界匹配）----
    const std::string code = StripLuaTrivia(ticket.source);
    std::vector<std::string> sandbox_issues;
    for (const std::string_view banned : kBannedApis) {
        ScanBanned(code, banned, &sandbox_issues);
    }
    report.sandbox_ok = sandbox_issues.empty();
    report.issues.insert(report.issues.end(), sandbox_issues.begin(), sandbox_issues.end());

    // ---- 冒烟执行：仍在**隔离临时 VM** 上，不动生产 VM ----
    auto isolated = CreateIsolated();
    if (!isolated) {
        report.smoke_ok = false;
        report.issues.emplace_back("internal: cannot create isolated lua vm");
    } else {
        const core::Result<ScriptId> loaded =
            isolated.Value()->Load(ticket.name, ticket.source);
        if (!loaded) {
            report.syntax_ok = false;
            report.smoke_ok = false;
            report.issues.push_back(std::string("smoke: load failed: ") +
                                    std::string(loaded.Err().Message()));
        } else if (SmokeCall(*isolated.Value(), loaded.Value())) {
            report.smoke_ok = true;
            report.smoke_calls = 1;
        } else {
            report.smoke_ok = false;
            report.issues.push_back(std::string("smoke: '") +
                                    std::string(config_.smoke_function) + "' failed");
        }
    }

    const core::SteadyNs elapsed_ns =
        core::MonotonicClock::Elapsed(t_begin, core::MonotonicClock::Point());
    report.elapsed = core::DurationMs{elapsed_ns / core::kSteadyNsPerMilli};
    if (report.elapsed > config_.validate_budget) {
        report.issues.push_back(
            "budget: validate took " +
            ToDecimal(static_cast<std::uint64_t>(report.elapsed.count())) + "ms (budget " +
            ToDecimal(static_cast<std::uint64_t>(config_.validate_budget.count())) + "ms)");
    }

    report.ok = report.syntax_ok && report.sandbox_ok && report.smoke_ok &&
                !(report.elapsed > config_.validate_budget);

    {
        std::lock_guard<std::mutex> lock(mu_);
        Entry& entry = entries_[ticket.name];
        entry.pending = ticket;
        entry.pending.report = report;
        entry.has_pending = report.ok;
        entry.state = report.ok ? ReloadState::PendingActivate : ReloadState::Failed;
        if (!report.ok) {
            failed_count_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return core::Result<ValidationReport>::Ok(report);
}

// ---------------------------------------------------------------------------
// 阶段 4：Activate（★ Tick Safe Point ★）
// ---------------------------------------------------------------------------

core::Result<void> HotReloader::ActivateLocked(Entry& entry, const ReloadTicket& ticket,
                                              core::TraceID trace, bool is_rollback,
                                              std::string_view actor) {
    const core::Result<ScriptId> swapped = SwapIn(ticket.name, ticket.source);
    if (!swapped) {
        entry.state = ReloadState::Failed;
        failed_count_.fetch_add(1, std::memory_order_relaxed);
        return core::Result<void>::Fail(swapped.Err());
    }
    entry.current_id = swapped.Value();

    ScriptVersion version;
    version.id = swapped.Value();
    version.version = entry.next_version++;
    version.checksum = ticket.checksum;
    version.activated_at_ms = core::WallClock::UnixMillis();
    version.activated_by.assign(actor);
    if (trace != core::kInvalidTraceId) {
        version.activated_by += "/trace=" + ToDecimal(trace);
    }

    entry.current = version;
    Entry::Past past;
    past.version = version;
    past.source = ticket.source;
    past.origin = ticket.origin;
    entry.history.insert(entry.history.begin(), std::move(past));
    // §21「禁止无限保留历史版本（内存泄漏）」：硬截断到 `max_rollback_versions`。
    if (entry.history.size() > config_.max_rollback_versions) {
        entry.history.resize(config_.max_rollback_versions);
    }

    entry.state = is_rollback ? ReloadState::RolledBack : ReloadState::Activated;
    // 回滚到的版本此前已验证过，无需再验（否则会陷入「验证失败→回滚→再验证」的循环）。
    entry.needs_verify = !is_rollback;
    entry.activated_tick = safe_point_tick_.load(std::memory_order_relaxed);
    entry.has_pending = false;

    RecordAudit(version, ticket.name);
    activated_count_.fetch_add(1, std::memory_order_relaxed);
    return core::Result<void>::Ok();
}

core::Result<void> HotReloader::Activate(const ReloadTicket& ticket, core::TraceID trace) {
    // §21「禁止在 Tick 执行中间替换脚本」→ 三重门，缺一不可。
    if (!InSafePoint()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "activate outside tick safe point", core::domain::kLua));
    }
    if (!context_.Vm().OnOwnerThread()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "activate not on lua owner thread", core::domain::kLua));
    }
    if (context_.InScriptExecution()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "activate during script execution", core::domain::kLua));
    }
    if (ticket.name.empty()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "empty script name", core::domain::kLua));
    }
    // §21「禁止未校验直接激活」的**唯一**判据是下面持锁时的内部记录，**不是** `ticket.report`。
    //
    // 【踩坑 · 入参 report 恒为 false】`Validate` 按 §7 冻结签名**按值**接收 `ReloadTicket`，
    //   它填好的 `report` 只留在本对象的 `entries_[name].pending` 里，**无法回传**给调用方
    //   手上那份副本。因此若在此处以 `ticket.report.ok` 为判据，标准流程
    //   「`Prepare` → `Validate` → `Activate`」会**必然**在 Activate 处报 UNAUTHORIZED
    //   （实测：`TestActivateGateAndThreadModel` 的 `CHECK_OK(Activate(prepared.Value()))` 直接红）。
    //   更关键的是，要求调用方手工把 `report` 回填进票证，等于把「是否已校验」交给调用方自述
    //   —— 那正是伪造开关。以内部 (name, checksum) 记录为判据既让标准流程自然通过，
    //   又顺带堵死「手上拿个 report.ok=true 的假票证直接激活」。
    //   下面注释还解释了为什么必须用 checksum 对齐。

    std::lock_guard<std::mutex> lock(mu_);
    if (config_.validate_before_activate) {
        // 本对象记录的「该脚本已通过校验的票证」才算数（理由见函数上半段的注释）。
        // checksum 对齐：同 checksum ⇔ 同源码，避免「校验了 A 却激活 B」。
        const Entry* entry = FindEntry(ticket.name);
        const bool approved = entry != nullptr && entry->has_pending &&
                              entry->pending.report.ok &&
                              entry->pending.checksum == ticket.checksum;
        if (!approved) {
            return core::Result<void>::Fail(core::Error(
                core::ErrorCode::UNAUTHORIZED, "ticket not validated", core::domain::kLua));
        }
    }
    Entry& entry = entries_[ticket.name];
    return ActivateLocked(entry, ticket, trace, /*is_rollback=*/false, "hot_reloader");
}

std::size_t HotReloader::ActivatePending(core::TraceID trace) {
    if (!InSafePoint() || !context_.Vm().OnOwnerThread() || context_.InScriptExecution()) {
        return 0;
    }
    std::vector<ReloadTicket> ready;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ready.reserve(entries_.size());
        for (auto& [name, entry] : entries_) {
            (void)name;
            if (entry.has_pending && entry.pending.report.ok) {
                ready.push_back(entry.pending);
            }
        }
    }
    std::size_t activated = 0;
    for (const ReloadTicket& ticket : ready) {
        if (Activate(ticket, trace)) {
            ++activated;
        }
    }
    return activated;
}

// ---------------------------------------------------------------------------
// 回滚
// ---------------------------------------------------------------------------

core::Result<void> HotReloader::Rollback(std::string_view name, core::TraceID trace) {
    if (!InSafePoint()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "rollback outside tick safe point", core::domain::kLua));
    }
    if (!context_.Vm().OnOwnerThread()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "rollback not on lua owner thread", core::domain::kLua));
    }
    if (context_.InScriptExecution()) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::BUSY, "rollback during script execution", core::domain::kLua));
    }

    std::lock_guard<std::mutex> lock(mu_);
    Entry* entry = FindEntry(name);
    if (entry == nullptr || entry->history.size() < 2) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "no previous version to roll back to", core::domain::kLua));
    }
    // 选「最近一个 checksum 与当前不同的版本」：直接取 history[1] 会在「刚回滚过」时
    // 选到与当前完全相同的源码，产生一次无意义的版本号自增。
    const Entry::Past* target = nullptr;
    for (std::size_t i = 1; i < entry->history.size(); ++i) {
        if (entry->history[i].version.checksum != entry->current.checksum) {
            target = &entry->history[i];
            break;
        }
    }
    if (target == nullptr) {
        return core::Result<void>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "no distinct previous version", core::domain::kLua));
    }

    ReloadTicket ticket;
    ticket.name.assign(name);
    ticket.source = target->source;
    ticket.checksum = target->version.checksum;
    ticket.origin = target->origin;
    ticket.compiled = true;
    ticket.report.ok = true;      // 该版本此前已上线并验证过
    ticket.report.syntax_ok = true;
    ticket.report.sandbox_ok = true;
    ticket.report.smoke_ok = true;
    return ActivateLocked(*entry, ticket, trace, /*is_rollback=*/true, "rollback");
}

// ---------------------------------------------------------------------------
// 阶段 5：VerifyPass（自动回滚）
// ---------------------------------------------------------------------------

std::size_t HotReloader::VerifyPass(core::TraceID trace) {
    if (!InSafePoint() || !context_.Vm().OnOwnerThread() || context_.InScriptExecution()) {
        return 0;
    }
    const std::uint64_t current_tick = safe_point_tick_.load(std::memory_order_relaxed);

    std::vector<std::string> to_verify;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& [name, entry] : entries_) {
            // 只验证「在**更早的 Tick** 激活」的脚本：同一 Tick 内刚切换就立刻调用，
            // 会把「切换瞬间」误当成稳定态，也让「切换前后不在同一 Tick」失去判据。
            if (entry.needs_verify && entry.activated_tick < current_tick) {
                to_verify.push_back(name);
            }
        }
    }

    std::size_t processed = 0;
    for (const std::string& name : to_verify) {
        std::lock_guard<std::mutex> lock(mu_);
        Entry* entry = FindEntry(name);
        if (entry == nullptr || !entry->needs_verify) {
            continue;
        }
        ++processed;
        if (SmokeCall(context_, entry->current_id)) {
            entry->needs_verify = false;
            continue;
        }
        // §19 / §20 验收 #4：运行期崩溃 → **自动回滚**并计数，Scene 继续跑。
        auto_rollback_count_.fetch_add(1, std::memory_order_relaxed);
        const Entry::Past* target = nullptr;
        for (std::size_t i = 1; i < entry->history.size(); ++i) {
            if (entry->history[i].version.checksum != entry->current.checksum) {
                target = &entry->history[i];
                break;
            }
        }
        if (target == nullptr) {
            entry->state = ReloadState::Failed;
            entry->needs_verify = false;
            continue;
        }
        ReloadTicket ticket;
        ticket.name = name;
        ticket.source = target->source;
        ticket.checksum = target->version.checksum;
        ticket.origin = target->origin;
        ticket.compiled = true;
        ticket.report.ok = true;
        (void)ActivateLocked(*entry, ticket, trace, /*is_rollback=*/true, "auto-rollback");
    }
    return processed;
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

const ScriptVersion* HotReloader::CurrentVersion(std::string_view name) const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    const Entry* entry = FindEntry(name);
    if (entry == nullptr || entry->current.version == 0) {
        return nullptr;
    }
    return &entry->current;
}

std::vector<ScriptVersion> HotReloader::History(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<ScriptVersion> out;
    const Entry* entry = FindEntry(name);
    if (entry == nullptr) {
        return out;
    }
    out.reserve(entry->history.size());
    for (const Entry::Past& past : entry->history) {
        out.push_back(past.version);
    }
    return out;
}

ReloadState HotReloader::StateOf(std::string_view name) const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    const Entry* entry = FindEntry(name);
    return entry == nullptr ? ReloadState::Idle : entry->state;
}

// ---------------------------------------------------------------------------
// 安全点门
// ---------------------------------------------------------------------------

void HotReloader::BeginSafePoint(std::uint64_t tick) noexcept {
    safe_point_tick_.store(tick, std::memory_order_relaxed);
    in_safe_point_.store(true, std::memory_order_relaxed);
}

void HotReloader::EndSafePoint() noexcept {
    in_safe_point_.store(false, std::memory_order_relaxed);
}

bool HotReloader::InSafePoint() const noexcept {
    return in_safe_point_.load(std::memory_order_relaxed);
}

std::uint64_t HotReloader::SafePointTick() const noexcept {
    return in_safe_point_.load(std::memory_order_relaxed)
               ? safe_point_tick_.load(std::memory_order_relaxed)
               : 0;
}

// ---------------------------------------------------------------------------
// 阶段 6：审计
// ---------------------------------------------------------------------------

void HotReloader::SetAuditSink(IAuditSink* sink) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    sink_ = sink;
}

std::size_t HotReloader::DrainAudit() {
    std::size_t written = 0;
    for (;;) {
        IAuditSink* sink = nullptr;
        std::pair<ScriptVersion, std::string> item;
        {
            std::lock_guard<std::mutex> lock(mu_);
            sink = sink_;
            if (sink == nullptr || audit_queue_.empty()) {
                return written;
            }
            item = audit_queue_.front();
        }
        const core::Result<void> appended = sink->Append(item.first, item.second);
        if (!appended) {
            // 落盘失败：记录**留在队列里**等下次重试，绝不静默丢审计（§21 禁止无版本记录的热更）。
            return written;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!audit_queue_.empty()) {
                audit_queue_.erase(audit_queue_.begin());
            }
        }
        ++written;
    }
}

// ---------------------------------------------------------------------------
// 观测
// ---------------------------------------------------------------------------

std::uint64_t HotReloader::ActivatedCount() const noexcept {
    return activated_count_.load(std::memory_order_relaxed);
}

std::uint64_t HotReloader::AutoRollbackCount() const noexcept {
    return auto_rollback_count_.load(std::memory_order_relaxed);
}

std::uint64_t HotReloader::FailedCount() const noexcept {
    return failed_count_.load(std::memory_order_relaxed);
}

std::size_t HotReloader::ScriptCount() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    return entries_.size();
}

std::size_t HotReloader::PendingCount() const noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    std::size_t pending = 0;
    for (const auto& [name, entry] : entries_) {
        (void)name;
        if (entry.has_pending) {
            ++pending;
        }
    }
    return pending;
}

}  // namespace mmo::script
