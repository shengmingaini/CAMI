// scripting/lua/src/bind_event.cpp —— TASK-031 · 事件绑定（§8 Event，§17 集成链路的一端）。
//
// 契约（§8 绑定面）
// ----------------
//   event.subscribe(name, fn)     订阅脚本回调；`name` 命中已注册事件桥时会**顺带**
//                                 在真实 `core::EventBus` 上装好对应 C++ 订阅（幂等）。
//   event.publish(name, ...)      把脚本事件经**真实** `core::EventBus` 发布（**异步**）。
//
// 内置事件桥（4 条实体生命周期事件，全部来自 TASK-011 的具体 C++ 事件类型）
// -------------------------------------------------------------------
//   "entity.created"    <- game::EntityCreated
//   "entity.destroyed"  <- game::EntityDestroyed
//   "component.attached"<- game::ComponentAttached
//   "component.detached"<- game::ComponentDetached
//
//   桥的意义：脚本只认**名字**，C++ 只认**类型**。桥负责把类型订阅与字段编解码接起来，
//   于是新增事件类型不需要改脚本，改脚本也不需要动 C++ —— 扩展点集中在
//   `RegisterEventBridge`（§27.4 注册表机制，不在 switch 里穷举）。
//
// 为什么 `event.publish` 用自持内存的 `ScriptEvent` 而不是表引用
// ----------------------------------------------------------
//   EventBus 的 `Publish` 只**入队**（异步），真正的派发由宿主在 Tick 的 Event 阶段
//   经 `Drain` 驱动。因此载荷必须跨 Tick 存活：`ScriptEvent` 全内联定长缓冲
//   （无指针、无 std::string、外带 `kCritical=false`），可平凡拷贝入队。
//   若直接把 Lua 表交给总线，表会在本次调用返回后失效 → 跨 Tick 悬垂。
//
// 内存披露（诚实报告，§22）：`sizeof(ScriptEvent) = 280B > EventSlot::kInlinePayload(32B)`，
//   因此**脚本发布的事件走 EventBus 的堆路径**（每事件一次分配）。这是有意的取舍：
//   事件本就异步且非逐 Tick 高频，互换安全性（自持内存）值得这一次分配；
//   同进程热路径上的脚本调用（skill/Buff 公式）不经过本文件，无分配。

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "internal.h"

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/entity/entity_events.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"
#include "mmo/script/script_event.h"

namespace mmo::script {
namespace {

// ---------------------------------------------------------------------------
// 事件字段 → Lua 表 的编解码
// ---------------------------------------------------------------------------

/// 实体创建 / 销毁事件（字段布局相同）。
void FillEntityLifecycle(TableWriter& out, const void* ev, const char* name) {
    if (ev == nullptr) {
        return;
    }
    out.SetStr("name", name);
    // 两个事件类型字段一致，用联合视图读取（两者都是 {id,type,scene} 三个字段的 POD）。
    const auto* e = static_cast<const game::EntityCreated*>(ev);
    out.SetInt("id", static_cast<std::int64_t>(e->id));
    out.SetInt("type", static_cast<std::int64_t>(e->type));
    out.SetInt("scene", static_cast<std::int64_t>(e->scene));
}

void FillEntityCreated(TableWriter& out, const void* ev) {
    FillEntityLifecycle(out, ev, "entity.created");
}

void FillEntityDestroyed(TableWriter& out, const void* ev) {
    // EntityDestroyed 与 EntityCreated 字段布局逐字节一致（均为 {EntityId, EntityType, SceneId}），
    // 故复用同一填表逻辑；若将来两者分叉，编译器不会报警 —— 故此处显式重新解释。
    FillEntityLifecycle(out, ev, "entity.destroyed");
}

/// 组件挂载 / 卸载事件（字段布局相同：{id, ComponentTypeId}）。
void FillComponentLifecycle(TableWriter& out, const void* ev, const char* name) {
    if (ev == nullptr) {
        return;
    }
    out.SetStr("name", name);
    const auto* e = static_cast<const game::ComponentAttached*>(ev);
    out.SetInt("id", static_cast<std::int64_t>(e->id));
    out.SetInt("component_type", static_cast<std::int64_t>(static_cast<std::uint16_t>(e->type)));
}

void FillComponentAttached(TableWriter& out, const void* ev) {
    FillComponentLifecycle(out, ev, "component.attached");
}

void FillComponentDetached(TableWriter& out, const void* ev) {
    FillComponentLifecycle(out, ev, "component.detached");
}

// ---------------------------------------------------------------------------
// 事件桥安装器（注册到 ScriptContext::RegisterEventBridge）
// ---------------------------------------------------------------------------

/// 通用桥：订阅具体 C++ 事件类型 → 按脚本事件名派发给脚本订阅者。
///
/// **必须**在 OwnerThread 上执行（`EventBus::Drain` 由宿主线程驱动，§9）；
/// `DispatchEventToSubscribers` 内部会再校验一次线程归属。
/// 返回该订阅 ID，由 ScriptContext 记录并在析构时退订 —— 否则捕获了 context 的
/// lambda 会在 context 销毁后被总线回调，形成悬垂（§19）。
template <typename TEvent>
core::Result<core::EventBus::SubId> InstallLifecycleBridge(core::EventBus& bus, ScriptContext& ctx,
                                                          const char* script_name,
                                                          void (*fill)(TableWriter&, const void*)) {
    ScriptContext* context = &ctx;
    return bus.Subscribe<TEvent>([context, script_name, fill](const TEvent& ev) {
        // 派发隔离（§19）：单个脚本订阅者出错/超限只记录，不打断 Drain，故忽略返回值。
        (void)context->DispatchEventToSubscribers(script_name, fill,
                                                 static_cast<const void*>(&ev));
    });
}

core::Result<core::EventBus::SubId> BridgeEntityCreated(core::EventBus& bus, ScriptContext& ctx,
                                                       std::string_view /*name*/, void* /*user*/) {
    return InstallLifecycleBridge<game::EntityCreated>(bus, ctx, "entity.created",
                                                      &FillEntityCreated);
}

core::Result<core::EventBus::SubId> BridgeEntityDestroyed(core::EventBus& bus, ScriptContext& ctx,
                                                         std::string_view /*name*/,
                                                         void* /*user*/) {
    return InstallLifecycleBridge<game::EntityDestroyed>(bus, ctx, "entity.destroyed",
                                                        &FillEntityDestroyed);
}

core::Result<core::EventBus::SubId> BridgeComponentAttached(core::EventBus& bus, ScriptContext& ctx,
                                                           std::string_view /*name*/,
                                                           void* /*user*/) {
    return InstallLifecycleBridge<game::ComponentAttached>(bus, ctx, "component.attached",
                                                          &FillComponentAttached);
}

core::Result<core::EventBus::SubId> BridgeComponentDetached(core::EventBus& bus, ScriptContext& ctx,
                                                           std::string_view /*name*/,
                                                           void* /*user*/) {
    return InstallLifecycleBridge<game::ComponentDetached>(bus, ctx, "component.detached",
                                                          &FillComponentDetached);
}

// ---------------------------------------------------------------------------
// Lua 侧入口
// ---------------------------------------------------------------------------

/// `event.subscribe(name, fn)`。
core::Result<int> EventSubscribe(ScriptCall& call, void* /*user*/) {
    if (call.ArgCount() != 2) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event.subscribe needs 2 args", core::domain::kLua));
    }
    if (!call.ArgIsString(0)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event name must be string", core::domain::kLua));
    }
    const std::string_view name = call.Arg(0).AsStr();
    if (name.empty() || name.size() > kScriptEventMaxNameLen) {
        // 超长拒绝而非截断：截断会让两个不同事件名撞成同一个（§8 禁止静默截断的同源要求）。
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event name invalid", core::domain::kLua));
    }
    const int fn_index = call.ArgStackIndex(1);
    ScriptContext& ctx = call.Context();
    lua_State* state = ctx.Vm().NativeState();
    if (fn_index < 0 || lua_isfunction(state, fn_index) == 0) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event handler must be function",
            core::domain::kLua));
    }

    // 先装 C++ 侧订阅（幂等）：装失败就不要登记脚本回调，避免出现「订阅了但永远收不到」的假象。
    const core::Result<void> ensured = ctx.EnsureEventSubscription(name);
    if (!ensured) {
        return core::Result<int>::Fail(ensured.Err());
    }

    lua_pushvalue(state, fn_index);
    const int ref = luaL_ref(state, LUA_REGISTRYINDEX);
    ctx.AddSubscriber(name, ref);

    call.SetResult(ScriptValue::Bool(true));
    return call.Done();
}

/// `event.publish(name, ...)`。
core::Result<int> EventPublish(ScriptCall& call, void* /*user*/) {
    core::EventBus* bus = call.Services().events;
    if (bus == nullptr) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event api not bound", core::domain::kLua));
    }
    if (call.ArgCount() < 1 || !call.ArgIsString(0)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event.publish needs name", core::domain::kLua));
    }
    const std::string_view name = call.Arg(0).AsStr();
    if (name.empty() || name.size() > kScriptEventMaxNameLen) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event name invalid", core::domain::kLua));
    }

    ScriptEvent ev;
    if (!ev.SetName(name)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "event name too long", core::domain::kLua));
    }

    ScriptArgs packed;
    if (!detail::PackArgs(call, 1, packed)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "too many event args", core::domain::kLua));
    }
    for (std::size_t i = 0; i < packed.Size(); ++i) {
        const ScriptValue v = packed.At(i);
        if (v.IsNil()) {
            // 拒绝 nil：数组位置语义下 nil 会打洞，令接收侧的下标对齐失去意义（§8 严谨性）。
            return core::Result<int>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT, "nil event arg rejected", core::domain::kLua));
        }
        if (!ev.PushArg(v)) {
            // 容量（4 个）/ 字符串长度（48B）超限：拒绝而非截断。
            return core::Result<int>::Fail(core::Error(
                core::ErrorCode::INVALID_ARGUMENT, "event arg rejected", core::domain::kLua));
        }
    }

    const core::Result<void> published = bus->Publish(ev);
    if (!published) {
        return core::Result<int>::Fail(published.Err());
    }
    call.SetResult(ScriptValue::Bool(true));
    return call.Done();
}

}  // namespace

// ---------------------------------------------------------------------------
// detail：ScriptEvent 的默认编解码（声明见 src/internal.h）
// ---------------------------------------------------------------------------

namespace detail {

void FillScriptEventArgs(TableWriter& out, const void* ev) {
    const auto* event = static_cast<const ScriptEvent*>(ev);
    if (event == nullptr) {
        return;
    }
    out.SetStr("name", event->Name());
    // 数组部分：**位置语义**（`event.publish(name, a, b, c)` → fn({a, b, c}) 的 [1],[2],[3]）。
    // 命名域字段只有 C++ 侧事件桥才有（见 FillEntityCreated / FillComponentAttached），
    // 脚本自发布事件不带命名域 —— 这一不对称在 docs/INTERFACE.md §5 已显式声明。
    for (std::size_t i = 0; i < event->ArgCount(); ++i) {
        const ScriptValue v = event->Arg(i);
        switch (v.Type()) {
            case ScriptValueType::Bool:
                (void)out.PushBool(v.AsBool());
                break;
            case ScriptValueType::Integer:
                (void)out.PushInt(v.AsInt());
                break;
            case ScriptValueType::Number:
                (void)out.PushNum(v.AsNum());
                break;
            case ScriptValueType::String:
                (void)out.PushStr(v.AsStr());
                break;
            case ScriptValueType::Nil:
                // publish 侧已拒绝 nil，故不可达；留空以便将来扩展不引入静默错位。
                break;
        }
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// 内置安装
// ---------------------------------------------------------------------------

core::Result<void> ScriptContext::InstallEventBindings() {
    // ---- 1. 注册 4 条实体生命周期事件桥（§17 集成测试的另一端）----
    const struct {
        const char* name;
        EventBridgeInstall install;
    } kBridges[] = {
        {"entity.created", &BridgeEntityCreated},
        {"entity.destroyed", &BridgeEntityDestroyed},
        {"component.attached", &BridgeComponentAttached},
        {"component.detached", &BridgeComponentDetached},
    };
    for (const auto& bridge : kBridges) {
        const core::Result<void> added = RegisterEventBridge(bridge.name, bridge.install, nullptr);
        if (!added) {
            return added;
        }
    }

    // ---- 2. 安装 Lua 侧入口 ----
    const core::Result<void> subscribe =
        InstallBindingEntry("event.subscribe", BindingKind::Event, &EventSubscribe, nullptr,
                            /*read_only=*/false,
                            "event.subscribe(name, fn)；命中事件桥时顺带装 C++ 侧订阅（幂等）");
    if (!subscribe) {
        return subscribe;
    }
    return InstallBindingEntry("event.publish", BindingKind::Event, &EventPublish, nullptr,
                               /*read_only=*/false,
                               "event.publish(name, ...)；经 EventBus 异步发布（自持内存载荷）");
}

}  // namespace mmo::script
