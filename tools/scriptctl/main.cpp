// tools/scriptctl/main.cpp —— TASK-032 · 脚本热更运维 CLI（§15 第 9 步）
//
// 五个子命令：reload / validate / rollback / history / status
//
// 用法：
//   scriptctl validate --script <file.lua> [--name <n>] [--no-smoke]
//   scriptctl reload   --script <file.lua> [--name <n>] [--commit] [--no-smoke]
//   scriptctl rollback --name <n> --script <prev-version.lua> [--commit]
//   scriptctl history  --name <n>
//   scriptctl status
//
// 公共选项：--audit <path>（默认 docs/script-versions.md）· --help
//
// 退出码（CI 门禁必须能区分，禁止一律 0）：
//   0 = 通过 / 成功
//   1 = 用法错误、文件缺失、内部错误（HotReloader 基础设施问题）
//   2 = **脚本被判定不合格**（校验失败 / 与审计日志记录的版本不一致）→ 应当阻断上线
//
// ⚠ 作用域（刻意如此，§4 State Owner）
// ----------------------------------
//   本工具是**同进程**工具：它自建一个 `ScriptContext` + `HotReloader`。因此 `--commit`
//   的激活只对这个工具自己的 VM 生效，**不能改已在线进程里的脚本** —— 同一实时状态只能有
//   一个权威写入者，跨进程改脚本必须走控制面 Command。
//   它真正的价值是：① 在 CI / 上线前把不合法的脚本挡在门外（`validate`，退出码 2）；
//   ② 以审计日志为唯一事实来源，给出 `status` / `history` / `rollback` 的可读视图与一致性校验。
//   在线进程内热更应由宿主用 `ScriptReloadStage` 接入（见 scripting/lua/docs/HOTRELOAD.md §4）。
//
// 输出走 std::printf（tools/ 不在 engine/ 的裸日志红线作用域内，与 tools/migrate/main.cpp 同口径）。

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/script/hot_reload/hot_reloader.h"
#include "mmo/script/lua_vm.h"
#include "mmo/script/script_context.h"

namespace {

namespace core = mmo::core;

using mmo::script::HotReloader;
using mmo::script::LuaLimits;
using mmo::script::ReloadTicket;
using mmo::script::ScriptContext;
using mmo::script::ValidationReport;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitRejected = 2;

constexpr const char* kDefaultAudit = "docs/script-versions.md";
constexpr const char* kDefaultSmoke = "__hot_smoke";

// ---------------------------------------------------------------------------
// 审计日志读取（与 src/hot_reload/audit_sink.cpp 的书写格式互为逆运算）
// ---------------------------------------------------------------------------

struct AuditRow {
    std::string name;
    std::uint32_t version{0};
    std::string checksum;
    std::int64_t at_ms{0};
    std::string by;
};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

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

bool ParseU64(std::string_view text, std::uint64_t* out) {
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

bool ParseI64(std::string_view text, std::int64_t* out) {
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

std::vector<AuditRow> LoadAudit(const std::string& path) {
    std::vector<AuditRow> rows;
    std::ifstream in(path);
    if (!in) {
        return rows;  // 首次运行：文件尚不存在 → 空表，不是错误
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::vector<std::string_view> cells = SplitRow(line);
        if (cells.size() < 5) {
            continue;
        }
        std::uint64_t version = 0;
        std::int64_t at_ms = 0;
        if (cells[0] == "脚本名" || cells[0].empty()) {
            continue;
        }
        if (!ParseU64(cells[1], &version) || !ParseI64(cells[3], &at_ms)) {
            continue;  // 分隔行
        }
        AuditRow row;
        row.name.assign(cells[0]);
        row.version = static_cast<std::uint32_t>(version);
        row.checksum.assign(cells[2]);
        row.at_ms = at_ms;
        row.by.assign(cells[4]);
        rows.push_back(std::move(row));
    }
    return rows;
}

/// 该脚本在日志里的**最新**记录（同一版本重复出现时取最后一条）；无记录返回 nullptr。
const AuditRow* LatestOf(const std::vector<AuditRow>& rows, std::string_view name) {
    const AuditRow* best = nullptr;
    for (const AuditRow& row : rows) {
        if (row.name != name) {
            continue;
        }
        if (best == nullptr || row.version >= best->version) {
            best = &row;
        }
    }
    return best;
}

/// 该脚本版本号**小于 `before_version`** 的最新记录（= 上一个版本）；无则 nullptr。
const AuditRow* PreviousOf(const std::vector<AuditRow>& rows, std::string_view name,
                           std::uint32_t before_version) {
    const AuditRow* best = nullptr;
    for (const AuditRow& row : rows) {
        if (row.name != name || row.version >= before_version) {
            continue;
        }
        if (best == nullptr || row.version > best->version) {
            best = &row;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// 选项
// ---------------------------------------------------------------------------

struct Options {
    std::string action;
    std::string script;   // --script 源码文件
    std::string name;     // --name 脚本名（缺省用文件名去路径去扩展名）
    std::string audit{kDefaultAudit};
    bool commit{false};
    bool no_smoke{false};
};

void PrintUsage() {
    std::printf(
        "scriptctl —— TASK-032 Lua 热更运维工具\n"
        "\n"
        "用法：\n"
        "  scriptctl validate --script <file.lua> [--name <n>] [--no-smoke]\n"
        "  scriptctl reload   --script <file.lua> [--name <n>] [--commit] [--no-smoke]\n"
        "  scriptctl rollback --name <n> --script <prev-version.lua> [--commit]\n"
        "  scriptctl history  --name <n>\n"
        "  scriptctl status\n"
        "\n"
        "选项：\n"
        "  --script <path>   脚本源码文件\n"
        "  --name <name>     脚本名（默认取文件 basename 去掉扩展名）\n"
        "  --audit <path>    审计日志路径（默认 %s）\n"
        "  --commit          真正执行激活；缺省只打印计划（dry-run）\n"
        "  --no-smoke        跳过冒烟执行（仅做语法 + 沙箱静态扫描）。\n"
        "                    适用于依赖宿主绑定（entity.* / skill.* 等）的游戏脚本 ——\n"
        "                    本工具没有宿主的绑定面，贸然冒烟会因「索引 nil」被误判不合格。\n"
        "  --help            显示本帮助\n"
        "\n"
        "退出码：0 通过 / 1 用法或内部错误 / 2 脚本被判定不合格\n"
        "\n"
        "作用域：同进程工具，--commit 只作用于本工具自己的 VM；在线进程内热更请用\n"
        "        ScriptReloadStage 接入（见 scripting/lua/docs/HOTRELOAD.md §4）。\n",
        kDefaultAudit);
}

std::string BaseNameNoExt(const std::string& path) {
    std::size_t slash = path.find_last_of("/\\");
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base.resize(dot);
    }
    return base;
}

bool ParseArgs(int argc, char** argv, Options* opt) {
    if (argc < 2) {
        return false;
    }
    opt->action = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return false;
        }
        if (arg == "--commit") {
            opt->commit = true;
            continue;
        }
        if (arg == "--no-smoke") {
            opt->no_smoke = true;
            continue;
        }
        if ((arg == "--script" || arg == "--name" || arg == "--audit") && i + 1 < argc) {
            const std::string value = argv[++i];
            if (arg == "--script") {
                opt->script = value;
            } else if (arg == "--name") {
                opt->name = value;
            } else {
                opt->audit = value;
            }
            continue;
        }
        std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
        return false;
    }
    if (opt->name.empty() && !opt->script.empty()) {
        opt->name = BaseNameNoExt(opt->script);
    }
    return true;
}

// ---------------------------------------------------------------------------
// 报告输出
// ---------------------------------------------------------------------------

void PrintReport(const ValidationReport& report) {
    std::printf(
        "report: ok=%d syntax=%d sandbox=%d smoke=%d smoke_calls=%u elapsed_ms=%lld issues=%zu\n",
        report.ok ? 1 : 0, report.syntax_ok ? 1 : 0, report.sandbox_ok ? 1 : 0,
        report.smoke_ok ? 1 : 0, report.smoke_calls,
        static_cast<long long>(report.elapsed.count()), report.issues.size());
    for (const std::string& issue : report.issues) {
        std::printf("  issue: %s\n", issue.c_str());
    }
}

/// 建一个 ScriptContext + 绑定好的 HotReloader（`--no-smoke` 时关掉冒烟）。
struct Env {
    std::unique_ptr<ScriptContext> ctx;
    std::unique_ptr<HotReloader> reloader;

    explicit Env(bool no_smoke) {
        auto created = ScriptContext::Create(LuaLimits{});
        if (!created) {
            return;
        }
        ctx = std::move(created).Value();
        HotReloader::Config config;
        if (no_smoke) {
            // 空冒烟函数名 ⇒ SmokeCall 直接返回 true（见 hot_reloader.cpp），
            // 静态扫描照常进行 —— 这正是「跳过冒烟但保留沙箱检查」的精确实现。
            config.smoke_function = "";
        }
        reloader = std::make_unique<HotReloader>(*ctx, config);
    }
    bool valid() const { return ctx != nullptr && reloader != nullptr; }
};

/// 跑一次「PrepareFromFile + Validate」。返回 true 表示脚本合格；rejected 区分退出码 2。
bool PrepareAndValidate(Env& env, const Options& opt, ReloadTicket* ticket,
                        ValidationReport* report, bool* rejected) {
    *rejected = false;
    const auto prepared = env.reloader->PrepareFromFile(opt.name, opt.script);
    if (!prepared) {
        std::fprintf(stderr, "prepare failed: %s: %.*s\n",
                     core::ToString(prepared.Err().Code()),
                     static_cast<int>(prepared.Err().Message().size()),
                     prepared.Err().Message().data());
        // 语法错误 / 顶层执行错 → 脚本本身有问题，属于「不合格」（退出码 2）。
        *rejected = prepared.Err().Code() == core::ErrorCode::INVALID_ARGUMENT;
        return false;
    }
    const auto validated = env.reloader->Validate(prepared.Value());
    if (!validated) {
        std::fprintf(stderr, "validate failed: %s\n",
                     core::ToString(validated.Err().Code()));
        return false;
    }
    *ticket = prepared.Value();
    *report = validated.Value();
    if (!report->ok) {
        *rejected = true;
        return false;
    }
    return true;
}

/// 在安全点内激活（同进程）。成功时打印新版本号。
int ActivateInSafePoint(Env& env, const ReloadTicket& ticket) {
    env.reloader->BeginSafePoint(1);
    const auto activated = env.reloader->Activate(ticket, core::kInvalidTraceId);
    env.reloader->EndSafePoint();
    if (!activated) {
        std::fprintf(stderr, "activate failed: %s: %.*s\n",
                     core::ToString(activated.Err().Code()),
                     static_cast<int>(activated.Err().Message().size()),
                     activated.Err().Message().data());
        return kExitUsage;
    }
    const auto* version = env.reloader->CurrentVersion(ticket.name);
    std::printf("activated: name=%s version=%u checksum=%s\n", ticket.name.c_str(),
                version != nullptr ? version->version : 0u, ticket.checksum.c_str());
    return kExitOk;
}

// ---------------------------------------------------------------------------
// 子命令
// ---------------------------------------------------------------------------

int CmdValidate(const Options& opt) {
    if (opt.script.empty()) {
        std::fprintf(stderr, "validate: --script is required\n");
        return kExitUsage;
    }
    Env env(opt.no_smoke);
    if (!env.valid()) {
        std::fprintf(stderr, "validate: cannot create lua vm\n");
        return kExitUsage;
    }
    ReloadTicket ticket;
    ValidationReport report;
    bool rejected = false;
    const bool ok = PrepareAndValidate(env, opt, &ticket, &report, &rejected);
    PrintReport(report);
    if (!ok) {
        std::printf("validate: REJECTED name=%s\n", opt.name.c_str());
        return rejected ? kExitRejected : kExitUsage;
    }
    std::printf("validate: OK name=%s checksum=%s origin=%s\n", ticket.name.c_str(),
                ticket.checksum.c_str(), ticket.origin.c_str());
    return kExitOk;
}

int CmdReload(const Options& opt) {
    if (opt.script.empty()) {
        std::fprintf(stderr, "reload: --script is required\n");
        return kExitUsage;
    }
    Env env(opt.no_smoke);
    if (!env.valid()) {
        std::fprintf(stderr, "reload: cannot create lua vm\n");
        return kExitUsage;
    }
    ReloadTicket ticket;
    ValidationReport report;
    bool rejected = false;
    const bool ok = PrepareAndValidate(env, opt, &ticket, &report, &rejected);
    PrintReport(report);
    if (!ok) {
        std::printf("reload: REJECTED name=%s (旧版本未受影响)\n", opt.name.c_str());
        return rejected ? kExitRejected : kExitUsage;
    }
    if (!opt.commit) {
        std::printf(
            "reload: PLAN name=%s checksum=%s origin=%s\n"
            "  将执行：安全点内 Activate（原子替换，不重建 VM）。\n"
            "  加 --commit 才会真正激活。\n",
            ticket.name.c_str(), ticket.checksum.c_str(), ticket.origin.c_str());
        return kExitOk;
    }
    return ActivateInSafePoint(env, ticket);
}

int CmdRollback(const Options& opt) {
    if (opt.name.empty() || opt.script.empty()) {
        std::fprintf(stderr, "rollback: --name and --script are both required\n");
        return kExitUsage;
    }
    const std::vector<AuditRow> rows = LoadAudit(opt.audit);
    const AuditRow* current = LatestOf(rows, opt.name);
    if (current == nullptr) {
        std::fprintf(stderr, "rollback: no audit record for '%s' in %s\n", opt.name.c_str(),
                     opt.audit.c_str());
        return kExitUsage;
    }
    const AuditRow* previous = PreviousOf(rows, opt.name, current->version);
    if (previous == nullptr) {
        std::fprintf(stderr, "rollback: '%s' has no previous version to roll back to\n",
                     opt.name.c_str());
        return kExitUsage;
    }
    Env env(opt.no_smoke);
    if (!env.valid()) {
        std::fprintf(stderr, "rollback: cannot create lua vm\n");
        return kExitUsage;
    }
    ReloadTicket ticket;
    ValidationReport report;
    bool rejected = false;
    if (!PrepareAndValidate(env, opt, &ticket, &report, &rejected)) {
        PrintReport(report);
        std::fprintf(stderr, "rollback: given source is not acceptable\n");
        return rejected ? kExitRejected : kExitUsage;
    }
    PrintReport(report);
    // 一致性硬校验：给的源码必须**就是**审计日志记录的上一个版本，否则拒绝 ——
    // 回滚到错的代码比不回滚更危险。
    if (ticket.checksum != previous->checksum) {
        std::fprintf(stderr,
                     "rollback: REFUSED checksum mismatch — given source=%s, "
                     "audit previous(v%u)=%s\n",
                     ticket.checksum.c_str(), previous->version, previous->checksum.c_str());
        return kExitRejected;
    }
    std::printf("rollback: TARGET name=%s from=v%u to=v%u checksum=%s at_ms=%lld by=%s\n",
                opt.name.c_str(), current->version, previous->version, previous->checksum.c_str(),
                static_cast<long long>(previous->at_ms), previous->by.c_str());
    if (!opt.commit) {
        std::printf("  加 --commit 才会真正执行（同进程激活，会记入审计为新版本）。\n");
        return kExitOk;
    }
    std::printf(
        "  note: 同进程工具只能在本进程内激活该源码；在线进程的回滚必须由宿主用\n"
        "        HotReloader::Rollback() 在安全点内执行（见 docs/HOTRELOAD.md §6）。\n");
    return ActivateInSafePoint(env, ticket);
}

int CmdHistory(const Options& opt) {
    if (opt.name.empty()) {
        std::fprintf(stderr, "history: --name is required\n");
        return kExitUsage;
    }
    const std::vector<AuditRow> rows = LoadAudit(opt.audit);
    std::size_t shown = 0;
    // 版本号降序（最新在上）。
    for (const AuditRow& row : rows) {
        if (row.name != opt.name) {
            continue;
        }
        ++shown;
    }
    if (shown == 0) {
        std::printf("history: no records for '%s' in %s\n", opt.name.c_str(), opt.audit.c_str());
        return kExitOk;
    }
    std::printf("history: name=%s records=%zu (新 → 旧)\n", opt.name.c_str(), shown);
    // 收集 + 排序（数据量小，直接选择排序，避免额外依赖）
    std::vector<const AuditRow*> mine;
    mine.reserve(shown);
    for (const AuditRow& row : rows) {
        if (row.name == opt.name) {
            mine.push_back(&row);
        }
    }
    for (std::size_t i = 0; i < mine.size(); ++i) {
        for (std::size_t j = i + 1; j < mine.size(); ++j) {
            if (mine[j]->version > mine[i]->version) {
                const AuditRow* tmp = mine[i];
                mine[i] = mine[j];
                mine[j] = tmp;
            }
        }
    }
    for (const AuditRow* row : mine) {
        std::printf("  v%-4u checksum=%s at_ms=%lld by=%s\n", row->version, row->checksum.c_str(),
                    static_cast<long long>(row->at_ms), row->by.c_str());
    }
    return kExitOk;
}

int CmdStatus(const Options& opt) {
    const std::vector<AuditRow> rows = LoadAudit(opt.audit);
    if (rows.empty()) {
        std::printf("status: no audit records in %s\n", opt.audit.c_str());
        return kExitOk;
    }
    // 去重收集脚本名（保持首次出现顺序）。
    std::vector<std::string> names;
    for (const AuditRow& row : rows) {
        bool seen = false;
        for (const std::string& n : names) {
            if (n == row.name) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            names.push_back(row.name);
        }
    }
    std::printf("status: audit=%s scripts=%zu records=%zu\n", opt.audit.c_str(), names.size(),
                rows.size());
    for (const std::string& name : names) {
        const AuditRow* latest = LatestOf(rows, name);
        std::size_t count = 0;
        for (const AuditRow& row : rows) {
            if (row.name == name) {
                ++count;
            }
        }
        if (latest != nullptr) {
            std::printf("  %-28s current=v%-4u checksum=%s at_ms=%lld by=%s records=%zu\n",
                        name.c_str(), latest->version, latest->checksum.c_str(),
                        static_cast<long long>(latest->at_ms), latest->by.c_str(), count);
        }
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    // `--help` / 无参数 / `-h`：只打印用法，退出码 0（这是正常请求，不是错误）。
    if (argc < 2 || std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        PrintUsage();
        return kExitOk;
    }
    Options opt;
    if (!ParseArgs(argc, argv, &opt)) {
        PrintUsage();
        return kExitUsage;
    }
    if (opt.action == "validate") {
        return CmdValidate(opt);
    }
    if (opt.action == "reload") {
        return CmdReload(opt);
    }
    if (opt.action == "rollback") {
        return CmdRollback(opt);
    }
    if (opt.action == "history") {
        return CmdHistory(opt);
    }
    if (opt.action == "status") {
        return CmdStatus(opt);
    }
    std::fprintf(stderr, "unknown subcommand: %s\n", opt.action.c_str());
    PrintUsage();
    return kExitUsage;
}
