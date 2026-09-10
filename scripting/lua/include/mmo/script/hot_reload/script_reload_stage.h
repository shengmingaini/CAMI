#pragma once

/// TASK-032 · TASK-013 适配器 —— 把「Tick Safe Point」接到调度器的阶段扩展点上。
///
/// ⚠ 架构缺口（**已上报，未擅改 TASK-013**）
/// ----------------------------------------
///   任务书 §15 第 4 步要求「向 TASK-013 SimulationScheduler 注册 Tick 边界钩子」，
///   但实测 TASK-013 的公开接口（`simulation_scheduler.h` / `tick_phase.h` /
///   `simulation_stage.h`）**没有**「八阶段全部完成之后」的一等钩子：
///     - `FireTick` 固定跑完 `kTickPhaseOrder` 的 8 个阶段，然后 `events_.Drain(...)` 收尾，
///       中间没有可注册的回调点；
///     - `RegisterStage` 对**同 Phase 重复注册直接报错**（§21 禁止静默覆盖），
///       而 `TickPhase` 只有 8 个值，不存在第 9 个「SafePoint」相位。
///
///   并且 §27.3 明令本任务只能改自身 `module` 子树、禁止在 TASK-013 `STATUS: DONE` 后
///   静默改其接口签名。因此本任务**不修改 TASK-013**，改为提供本适配器：
///   把安全点实现为**最后一个阶段**（默认 `TickPhase::Replication`）—— 轮到它执行时，
///   前面所有阶段都已跑完，紧接着就是事件 drain 与下一 Tick 的 Input，因而它就是 Tick 边界。
///
///   正确做法是后续给 TASK-013 增加一等 `SafePointHook`（走 Architecture Change Request），
///   届时本适配器可直接替换为那个钩子，`HotReloader` 侧无需改动。详见 docs/HOTRELOAD.md §8。
///
/// 宿主注意（重复 Phase）
/// ---------------------
///   若宿主已在目标 Phase（默认 Replication）注册了别的阶段，`RegisterStage` 会报
///   `duplicate phase`。此时不要改 TASK-013，而是改用 `HotReloader::BeginSafePoint()` /
///   `EndSafePoint()` 在自家 Tick 边界处直接开门（本适配器做的事，三行代码而已）。

#include <cstdint>
#include <string_view>

#include "mmo/game/sched/simulation_stage.h"
#include "mmo/game/sched/tick_phase.h"

#include "mmo/script/hot_reload/hot_reloader.h"

namespace mmo::script {

/// 在 Tick 边界开安全点并驱动热更流水线的阶段 4-5。
///
/// `Execute` 的动作（全部在安全点窗口内，且不含任何 IO）：
///   1. `BeginSafePoint(ctx.tick_number)`
///   2. `ActivatePending(trace)`  —— 阶段 4：替换所有已校验待激活的脚本
///   3. `VerifyPass(trace)`       —— 阶段 5：验证上一轮激活的脚本，失败自动回滚
///   4. `EndSafePoint()`
///
/// 审计落盘（阶段 6）**不在本阶段做**：那是文件 IO，必须由宿主在 Tick 之外调
/// `HotReloader::DrainAudit()`（§11）。
class ScriptReloadStage final : public game::ISimulationStage {
public:
    explicit ScriptReloadStage(HotReloader& reloader,
                               game::TickPhase phase = game::TickPhase::Replication,
                               core::TraceID trace = core::kInvalidTraceId) noexcept
        : reloader_(reloader), phase_(phase), trace_(trace) {}

    game::TickPhase Phase() const noexcept override { return phase_; }
    std::string_view Name() const noexcept override { return "ScriptReload"; }

    /// 在 Tick 边界开安全点、激活待生效票证、验证上一轮激活。
    /// 禁止在此阻塞 / 做 IO（继承 `ISimulationStage` 的 §10 约束）。
    void Execute(const game::SceneContext& ctx) override;

    /// 本阶段累计执行的激活次数（观测）。
    std::uint64_t Activations() const noexcept { return activations_; }
    /// 本阶段累计执行的验证次数（观测）。
    std::uint64_t Verifications() const noexcept { return verifications_; }
    /// 本阶段累计开启的安全点次数（应等于 Tick 数）。
    std::uint64_t SafePoints() const noexcept { return safe_points_; }

private:
    HotReloader& reloader_;
    game::TickPhase phase_;
    core::TraceID trace_;
    std::uint64_t activations_{0};
    std::uint64_t verifications_{0};
    std::uint64_t safe_points_{0};
};

}  // namespace mmo::script
