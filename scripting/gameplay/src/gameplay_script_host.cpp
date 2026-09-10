// scripting/gameplay/src/gameplay_script_host.cpp — TASK-033 桥接层实现
//
// 落地顺序（§15 第 1–7 步）
// ------------------------
//   1 契约：四入口 + ctx/payload 双通道（见 docs/README.md §4）
//   2 加载器：按 config/gameplay/scripts.json 绑定「系统钩子 ↔ 脚本」（本文件）
//   3–7 五类脚本：见 quest/ skill/ ai/ boss/ event/
//
// 装载路径（刻意复用 TASK-032 的六阶段流水线，而不是直接 `ScriptContext::Load`）
// ------------------------------------------------------------------------------
//   目的是让**首版脚本也进版本历史** —— 否则 `Rollback` 无「上一个版本」可用，
//   §20 #5「热更一个技能公式……且可回滚」就无法成立。
//   `ScriptContext::Load` 仍然要调一次：`ScriptContext` 只提供 `NameOf(id)`、**没有**
//   `IdOf(name)`（TASK-032 头内已注明），而四入口调用需要 id，故由 `Load` 取得句柄，
//   再由流水线把同一脚本纳入版本管理（`ReloadInPlace` 保持 id 不变）。
//
// 失败隔离（§19）
// --------------
//   单条脚本失败 ⇒ 只记录 + 卸载 + 不进路由表；其余脚本照常装载。
//   路由表里没有它 = 该 hook **回退到 C++ 默认行为**（不存在「半挂载」状态）。

#include "mmo/gameplay/gameplay_script_host.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>

#include "mmo/script/script_binding.h"

namespace mmo::gameplay {
namespace {

constexpr std::string_view kDomain = core::domain::kLua;

/// 装载期注册的命令 op（与 TASK-031 `src/bind_command.cpp` 发出的 op 名一一对应；§27.2 禁止改名）。
constexpr std::string_view kOps[] = {
    "skill.cast",
    "quest.set_progress",
    "quest.complete",
    "entity.set_hp",
};

/// 「清单 keys 为空 = 通配」的哨兵路由键。
///
/// 用静态存储期的字面量而不是成员 `std::string`：`routes_` 的键是 `string_view`，
/// 指向的存储必须比宿主长寿且不被改写（成员字符串一旦被赋值/移动就会悬垂）。
/// `'\x01'` 前缀保证不会与任何真实路由键（技能 id / 事件名 / 模板名）碰撞。
constexpr std::string_view kWildcardKey = "\x01*";

/// 读整个文件（Cold Path；§11 External IO 只允许在装载期发生）。
bool ReadWholeFile(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

core::Result<void> FailVoid(core::ErrorCode code, std::string what) {
    return core::Result<void>::Fail(core::Error(code, what, kDomain));
}

}  // namespace

// ===========================================================================
// 构造 / 生命周期
// ===========================================================================

GameplayScriptHost::GameplayScriptHost(const Config& config) : config_(config) {}

GameplayScriptHost::~GameplayScriptHost() = default;

core::Result<std::unique_ptr<GameplayScriptHost>> GameplayScriptHost::Create(const Config& config) {
    if (config.max_tick_hz == 0 || config.max_tick_hz > 1) {
        // §21「禁止高频（> 1Hz）调用周期脚本」——宿主配置层也拦一道。
        return core::Result<std::unique_ptr<GameplayScriptHost>>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "max_tick_hz 必须为 1（> 1Hz 违反 §21）", kDomain));
    }

    script::LuaLimits limits;
    limits.memory_bytes = config.memory_bytes;
    limits.max_instructions = config.max_instructions;
    limits.max_exec_time = config.max_exec_time;
    limits.max_stack_depth = config.max_stack_depth;
    limits.allow_io = false;         // 白名单沙箱：io/os 根本不开库（§21）
    limits.allow_loadstring = false; // 沙箱：禁 load/loadfile/dofile

    core::Result<std::unique_ptr<script::ScriptContext>> created =
        script::ScriptContext::Create(limits);
    if (!created) {
        return core::Result<std::unique_ptr<GameplayScriptHost>>::Fail(created.Err());
    }

    auto host = std::unique_ptr<GameplayScriptHost>(new GameplayScriptHost(config));
    host->context_ = std::move(created).Value();
    host->handle_ = "scene/" + std::to_string(config.scene_id);

    script::HotReloader::Config rc;  // 显式默认构造（避免 NSDMI + 嵌套默认实参的 GCC 坑）
    rc.validate_before_activate = true;
    rc.max_rollback_versions = 5;
    rc.validate_budget = core::DurationMs(50);
    rc.smoke_function = "__hot_smoke";  // 玩法脚本刻意不定义它 → 冒烟视为通过（见 README R2）
    rc.allow_filesystem_read = config.allow_filesystem_read;

    host->reloader_ = std::make_unique<script::HotReloader>(*host->context_, rc);
    // §15-3：隔离 VM 必须与生产 VM 同装配，否则冒烟执行会 `attempt to index a nil value`
    // 把**合法脚本全盘误拦**。这里装的是「无宿主状态」版绑定（见 PrepareIsolated 注释）。
    host->reloader_->SetIsolatedPreparer(&GameplayScriptHost::PrepareIsolated, host.get());
    return core::Result<std::unique_ptr<GameplayScriptHost>>::Ok(std::move(host));
}

// ===========================================================================
// 装配
// ===========================================================================

core::Result<void> GameplayScriptHost::BindEntityManager(game::EntityManager& entities) {
    entities_ = &entities;
    return context_->BindEntityApi(entities);
}

core::Result<void> GameplayScriptHost::BindEventBus(core::EventBus& events) {
    return context_->BindEventApi(events);
}

core::Result<void> GameplayScriptHost::BindCommandBus(core::CommandBus& commands) {
    return context_->BindCommandApi(commands);
}

void GameplayScriptHost::BindCommandHandler(GameplayCommandHandler handler, void* user) noexcept {
    handler_ = handler;
    handler_user_ = user;
}

void GameplayScriptHost::SetReferenceRegistry(const IReferenceRegistry* registry) noexcept {
    references_ = registry;
}

void GameplayScriptHost::SetAuditSink(script::IAuditSink* sink) noexcept {
    if (reloader_ != nullptr) {
        reloader_->SetAuditSink(sink);
    }
}

// ===========================================================================
// 原生绑定注册
// ===========================================================================

namespace {

core::Result<void> AddNativeBinding(script::ScriptContext& ctx, std::string_view name,
                                   script::NativeFn fn, void* user, bool read_only,
                                   std::string_view description) {
    script::BindingDef def;
    def.name.assign(name);
    def.kind = script::BindingKind::Native;
    def.fn = fn;
    def.user = user;
    def.read_only = read_only;
    def.description.assign(description);
    return ctx.AddBinding(std::move(def));
}

}  // namespace

core::Result<void> GameplayScriptHost::InstallGameplayBindings() {
    const core::Result<void> installed =
        AddNativeBinding(*context_, "gameplay.ctx", &GameplayScriptHost::BindingCtx, this, true,
                         "只读上下文表 {scene,tick,version,script,category,hook}");
    if (!installed) {
        return installed;
    }
    const core::Result<void> payload =
        AddNativeBinding(*context_, "gameplay.payload", &GameplayScriptHost::BindingPayload, this,
                         true, "本次事件的载荷表（宿主投递）");
    if (!payload) {
        return payload;
    }
    const core::Result<void> result =
        AddNativeBinding(*context_, "gameplay.result", &GameplayScriptHost::BindingResult, this,
                         false, "脚本回传结果（§7 出向通道，只认标量键）");
    if (!result) {
        return result;
    }
    const core::Result<void> hook =
        AddNativeBinding(*context_, "gameplay.hook", &GameplayScriptHost::BindingHook, this, true,
                         "当前 hook 名（字符串）");
    if (!hook) {
        return hook;
    }
    context_->FreezeBindings();  // 装配期结束：Tick 期绑定稳定（TASK-031 §10）
    return core::Result<void>::Ok();
}

core::Result<void> GameplayScriptHost::InstallOps() {
    for (const std::string_view op : kOps) {
        const core::Result<void> reg =
            context_->RegisterCommandOp(op, &GameplayScriptHost::CommandTrampoline, this);
        if (!reg) {
            // 重复装载（同一宿主第二次 LoadManifest）会命中「禁止静默覆盖」而报错：
            // op 只装一次，第二次刻意忽略（幂等装载）。
            continue;
        }
    }
    return core::Result<void>::Ok();
}

core::Result<script::ScriptReply> GameplayScriptHost::CommandTrampoline(
    const script::ScriptCommand& cmd, void* user) {
    GameplayScriptHost* self = Self(user);
    if (self == nullptr || self->handler_ == nullptr) {
        // §19「脚本调用不存在的 API 返回明确错误而非崩溃」——未注入状态 Owner 时明确报错。
        return core::Result<script::ScriptReply>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "no gameplay command handler bound", kDomain));
    }
    return self->handler_(cmd, self->handler_user_);
}

core::Result<void> GameplayScriptHost::PrepareIsolated(script::ScriptContext& ctx, void* user) {
    (void)user;
    // 隔离 VM 只承担**编译 + 静态扫描**（§15-3）。它不持有宿主实时状态：
    // 绑定以 `user = nullptr` 安装，任何实际调用都会得到明确错误，
    // 而不是读到另一线程正在改写的 `payload_` / `slot_`（那是数据竞争）。
    const core::Result<void> a = AddNativeBinding(
        ctx, "gameplay.ctx", &GameplayScriptHost::BindingCtx, nullptr, true, "隔离 VM：不可用");
    if (!a) {
        return a;
    }
    const core::Result<void> b = AddNativeBinding(
        ctx, "gameplay.payload", &GameplayScriptHost::BindingPayload, nullptr, true,
        "隔离 VM：不可用");
    if (!b) {
        return b;
    }
    const core::Result<void> c = AddNativeBinding(
        ctx, "gameplay.result", &GameplayScriptHost::BindingResult, nullptr, false,
        "隔离 VM：不可用");
    if (!c) {
        return c;
    }
    const core::Result<void> d = AddNativeBinding(
        ctx, "gameplay.hook", &GameplayScriptHost::BindingHook, nullptr, true, "隔离 VM：不可用");
    if (!d) {
        return d;
    }
    ctx.FreezeBindings();
    return core::Result<void>::Ok();
}

// ===========================================================================
// 装载
// ===========================================================================

std::string GameplayScriptHost::ResolvePath(std::string_view path) const {
    if (config_.root.empty() || path.empty() || path.front() == '/') {
        return std::string(path);
    }
    // 已带盘符（`F:/...`）时不重复前置。
    if (path.size() > 2 && path[1] == ':') {
        return std::string(path);
    }
    std::string out = config_.root;
    if (out.back() != '/' && out.back() != '\\') {
        out.push_back('/');
    }
    out.append(path);
    return out;
}

GameplayScriptHost::Entry* GameplayScriptHost::FindEntry(std::string_view name) noexcept {
    for (Entry& e : entries_) {
        if (e.binding.name == name) {
            return &e;
        }
    }
    return nullptr;
}

const GameplayScriptHost::Entry* GameplayScriptHost::FindEntry(std::string_view name) const noexcept {
    for (const Entry& e : entries_) {
        if (e.binding.name == name) {
            return &e;
        }
    }
    return nullptr;
}

std::string GameplayScriptHost::DescribeCallFailure(const core::Result<void>& result) const {
    if (result) {
        return {};
    }
    std::string message(result.Err().Message());
    const script::LuaVM::ErrorDetail& detail = context_->LastError();
    if (!detail.text.empty()) {
        message += " | lua: ";
        message += detail.text;
        if (detail.line > 0) {
            message += " (line " + std::to_string(detail.line) + ")";
        }
    }
    if (!detail.script.empty()) {
        message += " [script " + detail.script + "]";
    }
    return message;
}

core::Result<void> GameplayScriptHost::RequireEntry(script::ScriptId id, std::string_view fn,
                                                    std::string_view script_name, int mode,
                                                    double num_arg, std::int64_t int_arg) {
    core::Result<void> result = core::Result<void>::Fail(
        core::Error(core::ErrorCode::INTERNAL_ERROR, "unreachable", kDomain));
    switch (mode) {
        case 0:
            result = context_->Call(id, fn, handle_);
            break;
        case 1:
            result = context_->Call(id, fn, handle_, num_arg);
            break;
        default:
            result = context_->Call(id, fn, handle_, int_arg);
            break;
    }
    if (result) {
        return core::Result<void>::Ok();
    }
    // 区分「函数缺失」与「函数报错」：§7 要求四入口齐全，缺失必须报得清楚；
    // 而「函数存在但执行失败」是脚本的问题，处置不同（前者拒装，后者该次调用作废）。
    const bool missing = result.Err().Code() == core::ErrorCode::NOT_FOUND &&
                         result.Err().Message() == "script function not found";
    std::string what = "脚本 '" + std::string(script_name) + "' ";
    what += missing ? "缺少入口函数 " : "入口函数执行失败 ";
    what += std::string(fn);
    what += "：" ;
    what += DescribeCallFailure(result);
    return FailVoid(missing ? core::ErrorCode::INVALID_ARGUMENT : core::ErrorCode::INTERNAL_ERROR,
                    std::move(what));
}

core::Result<void> GameplayScriptHost::CheckEntryContract(const ScriptBinding& binding,
                                                          script::ScriptId id) {
    // `on_init` 是**真实调用**（装载即初始化，与 TASK-031 `Load` 语义一致）。
    const core::Result<void> init = RequireEntry(id, "on_init", binding.name, 0, 0.0, 0);
    if (!init) {
        return init;
    }
    // 其余三个入口用「哨兵入参」探针：既验证存在性，也顺带暴露装载期错误。
    payload_.Reset();
    if (!payload_.SetName(kContractProbe)) {
        return FailVoid(core::ErrorCode::INTERNAL_ERROR, "contract probe name too long");
    }
    const core::Result<void> ev = RequireEntry(id, "on_event", binding.name, 0, 0.0, 0);
    if (!ev) {
        return ev;
    }
    const core::Result<void> tk = RequireEntry(id, "on_tick", binding.name, 1, 0.0, 0);
    if (!tk) {
        return tk;
    }
    return RequireEntry(id, "on_reload", binding.name, 2, 0.0, 0);
}

core::Result<void> GameplayScriptHost::CheckReloadContract(const Entry& entry,
                                                           std::int64_t old_version) {
    payload_.Reset();
    if (!payload_.SetName(kContractProbe)) {
        return FailVoid(core::ErrorCode::INTERNAL_ERROR, "contract probe name too long");
    }
    const core::Result<void> ev =
        RequireEntry(entry.id, "on_event", entry.binding.name, 0, 0.0, 0);
    if (!ev) {
        return ev;
    }
    const core::Result<void> tk =
        RequireEntry(entry.id, "on_tick", entry.binding.name, 1, 0.0, 0);
    if (!tk) {
        return tk;
    }
    // 真实调用：§7「on_reload(ctx, old_version) —— 热更后的状态迁移」。
    return RequireEntry(entry.id, "on_reload", entry.binding.name, 2, 0.0, old_version);
}

core::Result<GameplayScriptHost::Loaded> GameplayScriptHost::LoadOne(const ScriptBinding& binding) {
    if (!config_.allow_filesystem_read) {
        return core::Result<Loaded>::Fail(core::Error(core::ErrorCode::UNAUTHORIZED,
                                                     "filesystem read disabled by config", kDomain));
    }
    const std::string path = ResolvePath(binding.path);
    std::string source;
    if (!ReadWholeFile(path, source)) {
        return core::Result<Loaded>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "无法读取脚本文件：" + path, kDomain));
    }

    // ---- 1) 取句柄（`ScriptContext` 没有 `IdOf(name)`，句柄只能由 Load 得到）----
    core::Result<script::ScriptId> loaded = context_->Load(binding.name, source);
    if (!loaded) {
        return core::Result<Loaded>::Fail(core::Error(loaded.Err().Code(),
                                                     "编译/装载失败：" + DescribeCallFailure(
                                                         core::Result<void>::Fail(loaded.Err())),
                                                     kDomain));
    }
    const script::ScriptId id = loaded.Value();

    // 一旦后续任一步失败，必须把已装入的脚本撤掉（禁止「半挂载」）。
    const auto reject = [&](core::ErrorCode code, std::string what) -> core::Result<Loaded> {
        (void)context_->Unload(id);
        return core::Result<Loaded>::Fail(core::Error(code, std::move(what), kDomain));
    };

    // ---- 2) 六阶段流水线（Prepare → Validate → Activate），让首版也进版本历史 ----
    core::Result<script::ReloadTicket> ticket = reloader_->Prepare(binding.name, source);
    if (!ticket) {
        return reject(ticket.Err().Code(),
                      "热更流水线 Prepare 失败：" + std::string(ticket.Err().Message()));
    }
    core::Result<script::ValidationReport> report = reloader_->Validate(ticket.Value());
    if (!report) {
        return reject(report.Err().Code(),
                      "热更流水线 Validate 失败：" + std::string(report.Err().Message()));
    }
    if (!report.Value().ok) {
        std::string detail = "校验未通过";
        for (const std::string& issue : report.Value().issues) {
            detail += " | ";
            detail += issue;
        }
        return reject(core::ErrorCode::INVALID_ARGUMENT, std::move(detail));
    }
    const core::Result<void> activated = reloader_->Activate(ticket.Value(), core::kInvalidTraceId);
    if (!activated) {
        return reject(activated.Err().Code(),
                      "热更流水线 Activate 失败：" + std::string(activated.Err().Message()));
    }

    // ---- 3) §7 契约校验（四入口齐全）+ on_init ----
    const core::Result<void> contract = CheckEntryContract(binding, id);
    if (!contract) {
        return reject(contract.Err().Code(), std::string(contract.Err().Message()));
    }

    Loaded out;
    out.id = id;
    out.version = 1;
    return core::Result<Loaded>::Ok(out);
}

core::Result<std::size_t> GameplayScriptHost::LoadManifest(const ScriptManifest& manifest) {
    // ---- 0) 清单自洽（在编译任何脚本之前做 → 失败时零副作用）----
    const std::vector<std::string> issues = ValidateManifest(manifest);
    if (!issues.empty()) {
        std::string detail = "清单校验失败（" + std::to_string(issues.size()) + " 项）：";
        detail += issues.front();
        if (issues.size() > 1) {
            detail += " 等";
        }
        return core::Result<std::size_t>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, std::move(detail), kDomain));
    }

    std::size_t enabled = 0;
    for (const ScriptBinding& b : manifest.scripts) {
        if (b.enabled) {
            ++enabled;
        }
    }
    if (enabled > config_.max_scripts) {
        return core::Result<std::size_t>::Fail(
            core::Error(core::ErrorCode::INVALID_ARGUMENT, "脚本数超过 max_scripts", kDomain));
    }

    // ---- 1) §19 外部引用校验（Quest / Boss 不存在 → 加载期报错）----
    bool any_requires = false;
    bool requires_unverified = false;
    for (const ScriptBinding& b : manifest.scripts) {
        if (b.requires_refs.empty()) {
            continue;
        }
        any_requires = true;
        for (const std::string& ref : b.requires_refs) {
            const std::size_t colon = ref.find(':');
            if (colon == std::string::npos) {
                continue;  // 格式错误已由 ValidateManifest 拦下
            }
            const std::string_view ns(ref.data(), colon);
            const std::string_view id(ref.data() + colon + 1, ref.size() - colon - 1);
            if (references_ == nullptr) {
                continue;
            }
            if (!references_->Exists(ns, id)) {
                return core::Result<std::size_t>::Fail(core::Error(
                    core::ErrorCode::NOT_FOUND,
                    "脚本 '" + b.name + "' 引用了不存在的 '" + ref + "'", kDomain));
            }
        }
    }
    if (any_requires && references_ == nullptr) {
        // 不静默放行：明确记录「本次未做引用校验」。
        // 【踩坑】这句提示必须**在**下面的 `load_errors_.clear()` 之后写入 ——
        // 早期版本把它放在步骤 1，结果被步骤 3 的清理一并抹掉，测试里
        // `LoadErrors().empty()` 恰好是「静默放行」的症状（单元测试已固化该断言）。
        requires_unverified = true;
    }

    // ---- 2) 装配（幂等）----
    (void)InstallOps();
    (void)InstallGameplayBindings();

    // ---- 3) 逐条装载（同一安全点内激活；启动期本就不在任何 Tick 中途）----
    manifest_ = manifest;
    entries_.clear();
    entries_.reserve(enabled);
    routes_.clear();
    load_errors_.clear();
    // 见步骤 1 的注释：这行必须在 clear 之后。
    if (requires_unverified) {
        load_errors_.push_back("requires: 未注入 IReferenceRegistry，引用校验已跳过");
    }
    stats_ = LoadStats{};
    stats_.total = manifest.scripts.size();
    stats_.enabled = enabled;

    BeginSafePoint(tick_);
    for (const ScriptBinding& b : manifest.scripts) {
        if (!b.enabled) {
            continue;
        }
        const core::Result<Loaded> one = LoadOne(b);
        if (!one) {
            ++stats_.failed;
            load_errors_.push_back(b.name + ": " + std::string(one.Err().Message()));
            continue;
        }
        Entry entry;
        entry.binding = b;
        entry.id = one.Value().id;
        entry.version = one.Value().version;
        entry.has_tick = b.tick_hz > 0.0;
        entries_.push_back(std::move(entry));
        ++stats_.loaded;
        ++stats_.by_category[static_cast<std::size_t>(b.category)];
    }
    EndSafePoint();

    // ---- 4) 路由表（key → entry 下标，清单顺序 = 短路顺序）----
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        const bool wildcard = e.binding.keys.empty();
        if (wildcard) {
            routes_[kWildcardKey].push_back(i);
            for (Hook h : e.binding.hooks) {
                (void)h;
                ++stats_.hooks;
            }
            continue;
        }
        for (const std::string& key : e.binding.keys) {
            RouteList& list = routes_[std::string_view(key)];
            if (std::find(list.begin(), list.end(), i) == list.end()) {
                list.push_back(i);
            }
        }
        stats_.hooks += e.binding.hooks.size() * e.binding.keys.size();
    }

    if (config_.require_all_scripts && stats_.failed > 0) {
        return core::Result<std::size_t>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT,
            "有 " + std::to_string(stats_.failed) + " 个脚本装载失败（require_all_scripts=true）：" +
                (load_errors_.empty() ? std::string() : load_errors_.front()),
            kDomain));
    }
    return core::Result<std::size_t>::Ok(stats_.loaded);
}

core::Result<std::size_t> GameplayScriptHost::LoadManifestFromFile(std::string_view path) {
    std::string error;
    core::Result<ScriptManifest> manifest = LoadManifestFile(path, &error);
    if (!manifest) {
        return core::Result<std::size_t>::Fail(manifest.Err());
    }
    return LoadManifest(manifest.Value());
}

// ===========================================================================
// 路由 + 调用
// ===========================================================================

void GameplayScriptHost::CollectRoutes(Hook hook, std::string_view key) const {
    scratch_.clear();
    const auto collect = [&](std::string_view k) {
        const auto it = routes_.find(k);
        if (it == routes_.end()) {
            return;
        }
        for (const std::size_t index : it->second) {
            const Entry& e = entries_[index];
            const bool serves =
                std::find(e.binding.hooks.begin(), e.binding.hooks.end(), hook) !=
                e.binding.hooks.end();
            if (!serves) {
                continue;
            }
            if (std::find(scratch_.begin(), scratch_.end(), index) == scratch_.end()) {
                scratch_.push_back(index);
            }
        }
    };
    if (!key.empty()) {
        collect(key);
    }
    collect(kWildcardKey);  // keys 为空的脚本对所有键生效
}

core::Result<void> GameplayScriptHost::InvokeEvent(const Entry& entry, Hook hook) {
    ctx_view_.scene = static_cast<std::int64_t>(config_.scene_id);
    ctx_view_.tick = static_cast<std::int64_t>(tick_);
    ctx_view_.version = static_cast<std::int64_t>(entry.version);
    ctx_view_.script = entry.binding.name;
    ctx_view_.category = ToString(entry.binding.category);
    ctx_view_.hook = ToString(hook);
    return context_->Call(entry.id, "on_event", handle_, payload_.Name());
}

core::Result<void> GameplayScriptHost::InvokeStr(const Entry& entry, std::string_view fn) {
    return context_->Call(entry.id, fn, handle_);
}

core::Result<void> GameplayScriptHost::InvokeNum(const Entry& entry, std::string_view fn, double v) {
    return context_->Call(entry.id, fn, handle_, v);
}

core::Result<void> GameplayScriptHost::InvokeInt(const Entry& entry, std::string_view fn,
                                                 std::int64_t v) {
    return context_->Call(entry.id, fn, handle_, v);
}

// ===========================================================================
// 五个系统钩子
// ===========================================================================

SkillFormulaResult GameplayScriptHost::ComputeSkillFormula(const SkillFormulaRequest& request) {
    SkillFormulaResult result;
    CollectRoutes(Hook::SkillFormula, request.skill_id);
    if (scratch_.empty()) {
        return result;  // matched=false → 调用方走 C++ 默认公式
    }
    result.matched = true;

    payload_.Reset();
    (void)payload_.SetName(HookEventName(Hook::SkillFormula));
    (void)payload_.AddStr("skill", request.skill_id);
    (void)payload_.AddInt("caster", request.caster);
    (void)payload_.AddInt("target", request.target);
    (void)payload_.AddNum("base", request.base);
    (void)payload_.AddNum("coeff", request.coefficient);
    (void)payload_.AddNum("ap", request.attack_power);
    (void)payload_.AddNum("defense", request.target_defense);
    (void)payload_.AddNum("hp_pct", request.target_hp_pct);
    (void)payload_.AddInt("level", request.caster_level);
    (void)payload_.AddInt("kind", request.kind);

    for (const std::size_t index : scratch_) {
        const Entry& entry = entries_[index];
        slot_.Reset();
        current_ = &entry;
        const core::Result<void> called = InvokeEvent(entry, Hook::SkillFormula);
        current_ = nullptr;
        if (!called) {
            ++script_errors_;  // §19 隔离：单脚本报错不影响其它脚本与本次 Tick
            continue;
        }
        ++hook_calls_[static_cast<std::size_t>(Hook::SkillFormula)];
        if (!slot_.has) {
            continue;  // 该脚本不认领这个 skill（载荷不匹配时的正常返回）
        }
        result.raw = slot_.d0;
        double clamped = 0.0;
        const char* reason = nullptr;
        if (!ClampFormulaAmount(result.raw, clamped, reason)) {
            // §19「脚本返回非法值（NaN / 负伤害）：C++ 侧校验并钳制」。
            result.clamped = true;
            result.reason = reason;
        }
        result.amount = clamped;
        result.ok = true;
        result.script = entry.binding.name;
        break;  // 短路：首个产出结果的脚本生效（清单顺序确定 → 结果确定）
    }
    return result;
}

std::size_t GameplayScriptHost::DispatchQuestEvent(const GameplayPayload& event) {
    CollectRoutes(Hook::QuestEvent, event.Name());
    if (scratch_.empty()) {
        return 0;
    }
    payload_ = event;
    std::size_t called = 0;
    // 任务事件**不是短路语义**：多个任务脚本可能都关心同一条事件。
    for (const std::size_t index : scratch_) {
        const Entry& entry = entries_[index];
        slot_.Reset();
        current_ = &entry;
        const core::Result<void> r = InvokeEvent(entry, Hook::QuestEvent);
        current_ = nullptr;
        if (!r) {
            ++script_errors_;
            continue;
        }
        ++hook_calls_[static_cast<std::size_t>(Hook::QuestEvent)];
        ++called;
    }
    return called;
}

AiDecision GameplayScriptHost::DecideAi(const AiContext& context) {
    AiDecision result;
    CollectRoutes(Hook::AiDecide, context.profile);
    if (scratch_.empty()) {
        return result;
    }
    result.matched = true;

    payload_.Reset();
    (void)payload_.SetName(HookEventName(Hook::AiDecide));
    (void)payload_.AddStr("profile", context.profile);
    (void)payload_.AddInt("self", context.self);
    (void)payload_.AddInt("target", context.target);
    (void)payload_.AddNum("hp_pct", context.hp_pct);
    (void)payload_.AddNum("distance", context.distance);
    (void)payload_.AddInt("state", context.state);

    for (const std::size_t index : scratch_) {
        const Entry& entry = entries_[index];
        slot_.Reset();
        current_ = &entry;
        const core::Result<void> called = InvokeEvent(entry, Hook::AiDecide);
        current_ = nullptr;
        if (!called) {
            ++script_errors_;
            continue;
        }
        ++hook_calls_[static_cast<std::size_t>(Hook::AiDecide)];
        if (!slot_.has) {
            continue;
        }
        bool clamped = false;
        result.action = ClampAiAction(slot_.i0, clamped);
        result.clamped = clamped;
        result.target = slot_.i1;
        result.ok = true;
        result.script = entry.binding.name;
        break;
    }
    return result;
}

BossPhaseDecision GameplayScriptHost::CheckBossPhase(const BossContext& context) {
    BossPhaseDecision result;
    CollectRoutes(Hook::BossPhase, context.boss_template);
    if (scratch_.empty()) {
        return result;
    }
    result.matched = true;
    result.phase = context.phase;

    payload_.Reset();
    (void)payload_.SetName(HookEventName(Hook::BossPhase));
    (void)payload_.AddStr("boss", context.boss_template);
    (void)payload_.AddInt("boss_id", context.boss);
    (void)payload_.AddNum("hp_pct", context.hp_pct);
    (void)payload_.AddInt("phase", context.phase);
    (void)payload_.AddInt("enraged", context.enraged);

    for (const std::size_t index : scratch_) {
        const Entry& entry = entries_[index];
        slot_.Reset();
        current_ = &entry;
        const core::Result<void> called = InvokeEvent(entry, Hook::BossPhase);
        current_ = nullptr;
        if (!called) {
            ++script_errors_;
            continue;
        }
        ++hook_calls_[static_cast<std::size_t>(Hook::BossPhase)];
        if (!slot_.has) {
            continue;
        }
        bool clamped = false;
        const std::int64_t phase = ClampBossPhase(slot_.i0, clamped);
        result.clamped = clamped;
        result.phase = phase;
        // C++ 是「是否真的切了阶段」的裁判：脚本声明 switched 且目标阶段与当前不同才算切。
        result.switched = slot_.b0 && phase != context.phase;
        result.skill_group = slot_.i1;
        result.summon_npc = slot_.i2;
        result.broadcast = slot_.b1;
        result.ok = true;
        result.script = entry.binding.name;
        break;
    }
    return result;
}

ActivityModifier GameplayScriptHost::QueryActivity(const ActivityQuery& query) {
    ActivityModifier result;
    CollectRoutes(Hook::ActivityModifier, query.activity_id);
    if (scratch_.empty()) {
        return result;
    }
    result.matched = true;

    payload_.Reset();
    (void)payload_.SetName(HookEventName(Hook::ActivityModifier));
    (void)payload_.AddStr("activity", query.activity_id);
    (void)payload_.AddInt("player", query.player);
    (void)payload_.AddInt("now_ms", query.now_ms);
    (void)payload_.AddInt("exp_gain", query.exp_gain);

    for (const std::size_t index : scratch_) {
        const Entry& entry = entries_[index];
        slot_.Reset();
        current_ = &entry;
        const core::Result<void> called = InvokeEvent(entry, Hook::ActivityModifier);
        current_ = nullptr;
        if (!called) {
            ++script_errors_;
            continue;
        }
        ++hook_calls_[static_cast<std::size_t>(Hook::ActivityModifier)];
        if (!slot_.has) {
            continue;
        }
        bool clamped = false;
        double exp_mult = 1.0;
        double drop_mult = 1.0;
        if (!ClampMultiplier(slot_.d0, exp_mult)) {
            clamped = true;
        }
        if (!ClampMultiplier(slot_.d1, drop_mult)) {
            clamped = true;
        }
        result.active = slot_.b0;
        result.exp_multiplier = exp_mult;
        result.drop_multiplier = drop_mult;
        result.expires_at_ms = slot_.i0;
        result.clamped = clamped;
        result.script = entry.binding.name;
        break;
    }
    return result;
}

std::size_t GameplayScriptHost::Tick(double dt_seconds) {
    ++tick_;
    if (!(dt_seconds > 0.0)) {
        return 0;
    }
    std::size_t fired = 0;
    for (Entry& entry : entries_) {
        if (!entry.has_tick || entry.binding.tick_hz <= 0.0) {
            continue;
        }
        // 清单已保证 tick_hz ≤ 1（ValidateManifest）→ period ≥ 1000ms。
        const double period_ms = 1000.0 / entry.binding.tick_hz;
        entry.tick_accum_ms += dt_seconds * 1000.0;
        if (entry.tick_accum_ms < period_ms) {
            continue;
        }
        // 丢弃积压（照 TASK-004 的教训：不设 `deadline = now + period` 会进入 catch-up 死亡螺旋）。
        entry.tick_accum_ms = 0.0;

        const std::uint32_t version = entry.version;
        slot_.Reset();
        current_ = &entry;
        const core::Result<void> r = InvokeNum(entry, "on_tick", dt_seconds);
        current_ = nullptr;
        if (!r) {
            ++script_errors_;
            continue;
        }
        if (entry.version != version) {
            // 周期调用期间发生了热更：本次 on_tick 已经跑完，不再补跑（下一次周期用新版本）。
            continue;
        }
        ++fired;
    }
    return fired;
}

// ===========================================================================
// 热更
// ===========================================================================

void GameplayScriptHost::BeginSafePoint(std::uint64_t tick) noexcept {
    if (reloader_ != nullptr) {
        reloader_->BeginSafePoint(tick);
    }
}

void GameplayScriptHost::EndSafePoint() noexcept {
    if (reloader_ != nullptr) {
        reloader_->EndSafePoint();
    }
}

bool GameplayScriptHost::InSafePoint() const noexcept {
    return reloader_ != nullptr && reloader_->InSafePoint();
}

core::Result<script::ReloadTicket> GameplayScriptHost::PrepareReload(std::string_view name,
                                                                    std::string_view source) {
    if (FindEntry(name) == nullptr) {
        // 热更不是装载：不在清单里的名字一律拒绝（禁止「热更」变成「偷偷加脚本」）。
        return core::Result<script::ReloadTicket>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "脚本不在清单中：" + std::string(name), kDomain));
    }
    return reloader_->Prepare(name, source);
}

core::Result<script::ReloadTicket> GameplayScriptHost::PrepareReloadFromFile(
    std::string_view name, std::string_view path) {
    if (FindEntry(name) == nullptr) {
        return core::Result<script::ReloadTicket>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "脚本不在清单中：" + std::string(name), kDomain));
    }
    const std::string resolved = ResolvePath(path);
    return reloader_->PrepareFromFile(name, resolved);
}

core::Result<script::ValidationReport> GameplayScriptHost::ValidateReload(
    const script::ReloadTicket& ticket) {
    return reloader_->Validate(ticket);
}

core::Result<void> GameplayScriptHost::ActivateReload(const script::ReloadTicket& ticket,
                                                      core::TraceID trace) {
    Entry* entry = FindEntry(ticket.name);
    if (entry == nullptr) {
        return FailVoid(core::ErrorCode::NOT_FOUND,
                        "脚本不在清单中：" + std::string(ticket.name));
    }
    const std::int64_t old_version = static_cast<std::int64_t>(entry->version);
    const core::Result<void> activated = reloader_->Activate(ticket, trace);
    if (!activated) {
        return activated;
    }
    entry->version += 1;

    // 新版本必须仍满足 §7 契约（四入口齐全），并执行状态迁移 `on_reload`。
    const core::Result<void> contract = CheckReloadContract(*entry, old_version);
    if (!contract) {
        // 契约破了：就地回滚，避免线上留一个缺入口的版本（§19「热更后脚本状态丢失」的对偶面）。
        (void)reloader_->Rollback(entry->binding.name, trace);
        entry->version -= 1;
        return contract;
    }
    return core::Result<void>::Ok();
}

core::Result<void> GameplayScriptHost::Rollback(std::string_view name, core::TraceID trace) {
    Entry* entry = FindEntry(name);
    if (entry == nullptr) {
        return FailVoid(core::ErrorCode::NOT_FOUND, "脚本不在清单中：" + std::string(name));
    }
    const core::Result<void> rolled = reloader_->Rollback(name, trace);
    if (!rolled) {
        return rolled;
    }
    if (entry->version > 1) {
        entry->version -= 1;
    }
    return core::Result<void>::Ok();
}

std::size_t GameplayScriptHost::VerifyPass(core::TraceID trace) {
    return reloader_->VerifyPass(trace);
}

std::size_t GameplayScriptHost::DrainAudit() { return reloader_->DrainAudit(); }

const script::ScriptVersion* GameplayScriptHost::CurrentVersion(std::string_view name) const noexcept {
    return reloader_->CurrentVersion(name);
}

// ===========================================================================
// 观测
// ===========================================================================

std::string_view GameplayScriptHost::PathOf(std::string_view name) const noexcept {
    const Entry* entry = FindEntry(name);
    return entry != nullptr ? std::string_view(entry->binding.path) : std::string_view{};
}

std::vector<std::string> GameplayScriptHost::ScriptNames() const {
    std::vector<std::string> names;
    names.reserve(entries_.size());
    for (const Entry& e : entries_) {
        names.push_back(e.binding.name);
    }
    return names;
}

std::size_t GameplayScriptHost::RouteCount(Hook hook) const noexcept {
    std::size_t count = 0;
    for (const auto& kv : routes_) {
        for (const std::size_t index : kv.second) {
            const Entry& e = entries_[index];
            if (std::find(e.binding.hooks.begin(), e.binding.hooks.end(), hook) !=
                e.binding.hooks.end()) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace mmo::gameplay
