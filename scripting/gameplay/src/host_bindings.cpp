// scripting/gameplay/src/host_bindings.cpp — TASK-033 §15.1 脚本契约的数据通道
//
// 为什么需要这四条绑定（而不是直接给 `Call` 传表）
// ----------------------------------------------
// TASK-031 的 `Call` 签名冻结为「标量参数」：`ScriptValue` 只有 Nil/Bool/Integer/Number/String，
// 没有 table 类型（`script_value.h` 明确「table 走 ScriptTableView 而非 ScriptValue」）。
// 而 §7 的四入口契约需要 `ctx` 与 `payload` 两张表。TASK-031 `docs/INTERFACE.md` §6 给了
// 两条**受控通路**把复杂数据带回 C++，本模块把同一机制**反向**用于「带表进脚本」：
//
//   入向（C++ → Lua）：`gameplay.ctx()` / `gameplay.payload()` —— 用 `SetResultTable` 返回表；
//   出向（Lua → C++）：`gameplay.result{…}` —— 一条 Native 绑定，宿主在自己的槽位上收集。
//
// 这样做的三个好处：
//   1. **不修改 TASK-031 冻结头**（§27.3 禁止跨任务改接口），也不需要宿主手搓 lua_State
//      （那会绕过 `ExecScope` 的指令/时间限额与 `run_depth_` 维护 → 限额形同虚设）；
//   2. 协议面**刻意收窄**：脚本只能拿到宿主愿意给的标量表，无法凭构造任意结构操纵上层语义；
//   3. 表数据的生命周期锁定在**本次同步调用**内，无跨 Tick 悬挂风险（§15）。
//
// 线程：绑定只在调用它的线程（= OwnerThread）执行；宿主侧状态由 `current_` 定位。

#include "mmo/gameplay/gameplay_script_host.h"

#include "mmo/script/script_binding.h"

namespace mmo::gameplay {
namespace {

/// 结果表里被识别的键（**协议面**：只认这些，其余忽略 —— 见 docs/README.md §4.3）。
constexpr std::string_view kKeyAmount = "amount";
constexpr std::string_view kKeyValue = "value";
constexpr std::string_view kKeyAction = "action";
constexpr std::string_view kKeyTarget = "target";
constexpr std::string_view kKeyPhase = "phase";
constexpr std::string_view kKeySwitched = "switched";
constexpr std::string_view kKeySkillGroup = "skill_group";
constexpr std::string_view kKeySummon = "summon";
constexpr std::string_view kKeyBroadcast = "broadcast";
constexpr std::string_view kKeyActive = "active";
constexpr std::string_view kKeyExpMult = "exp_mult";
constexpr std::string_view kKeyDropMult = "drop_mult";
constexpr std::string_view kKeyExpiresAt = "expires_at";

}  // namespace

namespace {

/// 隔离 VM（TASK-032 §15-3）里绑定以 `user = nullptr` 安装：它只承担编译 + 静态扫描，
/// **不持有宿主实时状态**。任何实际调用都返回明确错误，而不是读到另一线程正在改写的
/// `payload_` / `slot_`（那会是数据竞争）。
constexpr std::string_view kIsolatedMessage =
    "gameplay.* 不可用：隔离 VM 只做编译校验，不承载宿主状态（TASK-032 §15-3）";

core::Result<int> IsolatedUnavailable(script::ScriptCall& call) {
    (void)call;
    return core::Result<int>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                              kIsolatedMessage, core::domain::kLua));
}

}  // namespace

// ===========================================================================
// `gameplay.ctx()` —— 只读上下文表
// ===========================================================================

void GameplayScriptHost::FillCtx(script::TableWriter& out, void* user) {
    const auto* self = Self(user);
    if (self == nullptr) {
        out.SetStr("error", kIsolatedMessage);
        return;
    }
    out.SetInt("scene", self->ctx_view_.scene);
    out.SetInt("tick", self->ctx_view_.tick);
    out.SetInt("version", self->ctx_view_.version);
    out.SetStr("script", self->ctx_view_.script);
    out.SetStr("category", self->ctx_view_.category);
    out.SetStr("hook", self->ctx_view_.hook);
}

core::Result<int> GameplayScriptHost::BindingCtx(script::ScriptCall& call, void* user) {
    if (user == nullptr) {
        return IsolatedUnavailable(call);
    }
    call.SetResultTable(&GameplayScriptHost::FillCtx, user);
    return call.Done();
}

// ===========================================================================
// `gameplay.payload()` —— 本次事件的载荷表
// ===========================================================================

void GameplayScriptHost::FillPayload(script::TableWriter& out, void* user) {
    const auto* self = Self(user);
    if (self == nullptr) {
        out.SetStr("error", kIsolatedMessage);
        return;
    }
    const GameplayPayload& p = self->payload_;
    if (!p.Name().empty()) {
        out.SetStr("event", p.Name());
    }
    for (std::size_t i = 0; i < p.FieldCount(); ++i) {
        const ScalarField* f = p.Field(i);
        if (f == nullptr) {
            break;
        }
        switch (f->kind) {
            case FieldKind::Integer: out.SetInt(f->Name(), f->i); break;
            case FieldKind::Number: out.SetNum(f->Name(), f->n); break;
            case FieldKind::Bool: out.SetBool(f->Name(), f->b); break;
            case FieldKind::String: out.SetStr(f->Name(), f->Text()); break;
        }
    }
}

core::Result<int> GameplayScriptHost::BindingPayload(script::ScriptCall& call, void* user) {
    if (user == nullptr) {
        return IsolatedUnavailable(call);
    }
    call.SetResultTable(&GameplayScriptHost::FillPayload, user);
    return call.Done();
}

// ===========================================================================
// `gameplay.hook()` —— 当前 hook 名（脚本侧不必解析 ctx 表）
// ===========================================================================

core::Result<int> GameplayScriptHost::BindingHook(script::ScriptCall& call, void* user) {
    if (user == nullptr) {
        return IsolatedUnavailable(call);
    }
    const auto* self = Self(user);
    call.SetResult(script::ScriptValue::Str(self->ctx_view_.hook));
    return call.Done();
}

// ===========================================================================
// `gameplay.result{…}` —— 脚本回传结果（§7「C++ 只认四个入口」的出向通道）
// ===========================================================================

core::Result<int> GameplayScriptHost::BindingResult(script::ScriptCall& call, void* user) {
    if (user == nullptr) {
        return IsolatedUnavailable(call);
    }
    auto* self = Self(user);

    const script::ScriptTableView t = call.ArgTable(0);
    if (!t.Valid()) {
        // 容忍非表入参：脚本传错类型不应当让整次调用崩掉（§19「禁止脚本错误导致 Scene 崩溃」）。
        // 结果保持「未产出」，由调用方按 `ok == false` 处理。
        return call.Done();
    }

    ResultSlot& s = self->slot_;
    bool any = false;

    if (t.Has(kKeyAmount)) {
        s.d0 = t.GetNum(kKeyAmount, 0.0);
        any = true;
    }
    if (t.Has(kKeyValue)) {
        s.d0 = t.GetNum(kKeyValue, 0.0);
        any = true;
    }
    if (t.Has(kKeyAction)) {
        s.i0 = t.GetInt(kKeyAction, 0);
        any = true;
    }
    if (t.Has(kKeyTarget)) {
        s.i1 = t.GetInt(kKeyTarget, 0);
        any = true;
    }
    if (t.Has(kKeyPhase)) {
        s.i0 = t.GetInt(kKeyPhase, 1);
        any = true;
    }
    if (t.Has(kKeySkillGroup)) {
        s.i1 = t.GetInt(kKeySkillGroup, -1);
        any = true;
    }
    if (t.Has(kKeySummon)) {
        s.i2 = t.GetInt(kKeySummon, 0);
        any = true;
    }
    if (t.Has(kKeySwitched)) {
        s.b0 = t.GetBool(kKeySwitched, false);
        any = true;
    }
    if (t.Has(kKeyBroadcast)) {
        s.b1 = t.GetBool(kKeyBroadcast, false);
        any = true;
    }
    if (t.Has(kKeyActive)) {
        s.b0 = t.GetBool(kKeyActive, false);
        any = true;
    }
    if (t.Has(kKeyExpMult)) {
        s.d0 = t.GetNum(kKeyExpMult, 1.0);
        any = true;
    }
    if (t.Has(kKeyDropMult)) {
        s.d1 = t.GetNum(kKeyDropMult, 1.0);
        any = true;
    }
    if (t.Has(kKeyExpiresAt)) {
        s.i0 = t.GetInt(kKeyExpiresAt, 0);
        any = true;
    }

    s.has = any;
    return call.Done();
}

}  // namespace mmo::gameplay
