// scripting/lua/src/bind_entity.cpp —— TASK-031 · Entity 绑定（§8 Entity 面，§21 禁止裸指针）。
//
// 契约（§8 绑定面）
// ----------------
//   entity.get(id)      -> {id, type, scene, alive, version}        只读直查 EntityManager
//   entity.get_pos(id)  -> {x, y, z, yaw}                          只读直查
//   entity.set_hp(id,v) -> {ok,i0,i1,d0}                           **受控写**：经 CommandBus
//
// 为什么 `get` / `get_pos` 可以直连、`set_hp` 必须走命令
// ----------------------------------------------------
//   - 读：`EntityManager::Find` 是 O(1) 且无副作用，直连不需要引入总线开销（§12 热路径红线
//     只禁止「同步远程调用与 IO」，不禁止同进程内存读）。
//   - 写：HP 不是 Entity 的字段（Entity 是固定大小的池化结构，不含业务数值，见 entity.h）；
//     HP 的权威 Owner 是 Role 模块。脚本**不得**持有 C++ 对象指针写入（§21），因此
//     `set_hp` 只表达「请求扣血」，由状态 Owner 执行命令。
//
// 关键安全性质：脚本拿到的是 EntityId（整数句柄）而不是 `game::Entity*`。
// 失效 id（已销毁 / 世代不匹配）在 `Find` 里必然返回 nullptr → 本文件转成 `NOT_FOUND`
// 而非野指针解引用（§19 / §20.1）。

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "internal.h"

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/entity/entity_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/script/script_binding.h"
#include "mmo/script/script_context.h"

namespace mmo::script {
namespace {

/// `entity.get` 的结果视图（栈上瞬时对象，仅活到 SetResultTable 的 fill 调用内）。
struct EntityView {
    std::int64_t id{0};
    std::int64_t type{0};
    std::int64_t scene{0};
    std::int64_t version{0};
    bool alive{false};
};

void FillEntityView(TableWriter& out, void* user) {
    const auto* view = static_cast<const EntityView*>(user);
    if (view == nullptr) {
        return;
    }
    out.SetInt("id", view->id);
    out.SetInt("type", view->type);
    out.SetInt("scene", view->scene);
    out.SetBool("alive", view->alive);
    out.SetInt("version", view->version);
}

/// `entity.get_pos` 的结果视图。
struct PosView {
    double x{0.0};
    double y{0.0};
    double z{0.0};
    double yaw{0.0};
};

void FillPosView(TableWriter& out, void* user) {
    const auto* view = static_cast<const PosView*>(user);
    if (view == nullptr) {
        return;
    }
    out.SetNum("x", view->x);
    out.SetNum("y", view->y);
    out.SetNum("z", view->z);
    out.SetNum("yaw", view->yaw);
}

/// 取第 0 个参数为 EntityId。
///
/// 只接受**整数**：字符串化 id（如 "0x100000001"）被拒绝 —— 避免出现
/// 「同一个 id 两种写法」这种后续无法审计的接口面（§30 Correctness）。
core::Result<game::EntityId> ArgEntityId(const ScriptCall& call) {
    if (call.ArgCount() < 1) {
        return core::Result<game::EntityId>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "entity id required", core::domain::kLua));
    }
    if (!call.ArgIsInteger(0)) {
        return core::Result<game::EntityId>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "entity id must be integer", core::domain::kLua));
    }
    const std::int64_t raw = call.Arg(0).AsInt();
    if (raw <= 0) {
        // EntityId 由 (index<<32)|generation 合成，generation 从 1 起 ⇒ 0 永不合法。
        return core::Result<game::EntityId>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "entity id must be positive", core::domain::kLua));
    }
    return core::Result<game::EntityId>::Ok(static_cast<game::EntityId>(raw));
}

/// 共享前置：取 EntityManager 并按 id 查出实体（含统一错误语义）。
core::Result<game::Entity*> ResolveEntity(const ScriptCall& call) {
    game::EntityManager* manager = call.Services().entities;
    if (manager == nullptr) {
        return core::Result<game::Entity*>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "entity api not bound", core::domain::kLua));
    }
    const core::Result<game::EntityId> id = ArgEntityId(call);
    if (!id) {
        return core::Result<game::Entity*>::Fail(id.Err());
    }
    game::Entity* entity = manager->Find(id.Value());
    if (entity == nullptr) {
        // §19：脚本持有失效 EntityId → 明确 NOT_FOUND，绝不是野指针、也不是静默 nil。
        return core::Result<game::Entity*>::Fail(core::Error(
            core::ErrorCode::NOT_FOUND, "entity not found", core::domain::kLua));
    }
    return core::Result<game::Entity*>::Ok(entity);
}

/// `entity.get(id)` —— 只读直查（§8）。
core::Result<int> EntityGet(ScriptCall& call, void* /*user*/) {
    const core::Result<game::Entity*> resolved = ResolveEntity(call);
    if (!resolved) {
        return core::Result<int>::Fail(resolved.Err());
    }
    const game::Entity& entity = *resolved.Value();
    EntityView view{
        static_cast<std::int64_t>(entity.Id()),
        static_cast<std::int64_t>(entity.Type()),
        static_cast<std::int64_t>(entity.Scene()),
        static_cast<std::int64_t>(entity.Version()),
        entity.Alive(),
    };
    call.SetResultTable(&FillEntityView, static_cast<void*>(&view));
    return call.Done();
}

/// `entity.get_pos(id)` —— 只读直查（§8）。
core::Result<int> EntityGetPos(ScriptCall& call, void* /*user*/) {
    const core::Result<game::Entity*> resolved = ResolveEntity(call);
    if (!resolved) {
        return core::Result<int>::Fail(resolved.Err());
    }
    const game::Position& pos = resolved.Value()->Pos();
    PosView view{static_cast<double>(pos.x), static_cast<double>(pos.y),
                 static_cast<double>(pos.z), static_cast<double>(pos.yaw)};
    call.SetResultTable(&FillPosView, static_cast<void*>(&view));
    return call.Done();
}

/// `entity.set_hp(id, value)` —— **受控写**：只表达请求，真正的写入由状态 Owner 执行。
core::Result<int> EntitySetHp(ScriptCall& call, void* /*user*/) {
    if (call.ArgCount() != 2) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "entity.set_hp needs 2 args", core::domain::kLua));
    }
    if (!call.ArgIsInteger(0) || !call.ArgIsInteger(1)) {
        return core::Result<int>::Fail(core::Error(
            core::ErrorCode::INVALID_ARGUMENT, "entity.set_hp needs integers",
            core::domain::kLua));
    }
    // 先校验目标实体**当前存在**：否则一条指向已销毁实体的命令会一路走到业务系统，
    // 由业务系统各自决定「静默忽略」还是「报错」—— 契约就散了。统一在这里判成
    // NOT_FOUND，与 entity.get 同一口径（§19 失效 EntityId → NOT_FOUND）。
    const core::Result<game::Entity*> resolved = ResolveEntity(call);
    if (!resolved) {
        return core::Result<int>::Fail(resolved.Err());
    }
    // 走命令通道：op = "entity.set_hp"（== 绑定名），由宿主注册的 op handler 落实写。
    return detail::DispatchScriptCommand(call);
}

}  // namespace

core::Result<void> ScriptContext::InstallEntityBindings() {
    const core::Result<void> get =
        InstallBindingEntry("entity.get", BindingKind::Entity, &EntityGet, nullptr,
                            /*read_only=*/true,
                            "entity.get(id) -> {id,type,scene,alive,version}；只读直查");
    if (!get) {
        return get;
    }
    const core::Result<void> pos =
        InstallBindingEntry("entity.get_pos", BindingKind::Entity, &EntityGetPos, nullptr,
                            /*read_only=*/true, "entity.get_pos(id) -> {x,y,z,yaw}；只读直查");
    if (!pos) {
        return pos;
    }
    // 受控写：read_only = false，但实体不会因此被脚本直接修改 —— 写入经状态 Owner（§8）。
    return InstallBindingEntry("entity.set_hp", BindingKind::Entity, &EntitySetHp, nullptr,
                               /*read_only=*/false,
                               "entity.set_hp(id, value) -> {ok,...}；经 CommandBus 受控写");
}

}  // namespace mmo::script
