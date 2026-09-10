// scripting/lua/src/hot_reload/script_reload_stage.cpp —— TASK-032 · TASK-013 适配器实现。
//
// 见头文件顶部「架构缺口」说明：TASK-013 的 `FireTick` 没有「八阶段之后」的一等钩子，
// 且 `RegisterStage` 拒绝重复 Phase，故本阶段以**最后一个阶段**的身份充当 Tick 边界。

#include "mmo/script/hot_reload/script_reload_stage.h"

#include "mmo/game/scene/scene_context.h"

namespace mmo::script {

void ScriptReloadStage::Execute(const game::SceneContext& ctx) {
    // §13 / §9：安全点 = Tick 边界。本阶段按约定注册在最后一个 Phase，
    // 因此「本次 Execute 期间」即「Tick 八阶段收尾、下一 Tick Input 之前」。
    reloader_.BeginSafePoint(ctx.tick_number);
    ++safe_points_;

    // 阶段 4：把已校验待激活的脚本一次性切换（内部仍逐脚本替换，但都在同一个安全点内，
    // 因此不会出现「一半新一半旧被同一 Tick 读到」的中间态）。
    activations_ += reloader_.ActivatePending(trace_);

    // 阶段 5：验证**上一 Tick**激活的脚本；失败者自动回滚（§19 / §20 验收 #4）。
    verifications_ += reloader_.VerifyPass(trace_);

    reloader_.EndSafePoint();

    // 阶段 6（审计落盘）**刻意不在这里做**：那是文件 IO，出现在 Tick 内既违反 §11，
    // 也必然顶破 §22 的 <100us 停顿预算。由宿主在 Tick 之外调 `DrainAudit()`。
}

}  // namespace mmo::script
