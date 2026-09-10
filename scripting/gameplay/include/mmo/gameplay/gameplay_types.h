#pragma once

/// TASK-033 · 玩法脚本的类型契约（§7 / §8）
///
/// 设计约束
/// --------
///   - 本头**只含纯值类型**：不含 Lua 头、不含业务模块头（combat/quest/ai），
///     因此下游（gamenode 装配层）能依赖本头而不被 Lua 版本或业务模块耦合（§27.3）。
///   - 所有载荷容器为**固定容量内联数组**：技能公式在 Tick 热路径被调用（§10），
///     载荷编解码禁止堆分配。
///   - 「C++ 只认四个入口函数」（§7），脚本名/路径/绑定关系全部来自
///     `config/gameplay/scripts.json`（§21 禁止硬编码）。
///
/// 线程：本头的类型只在拥有 ScriptContext 的 SimulationThread 上使用（§9）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mmo::gameplay {

// ===========================================================================
// §8 五类脚本 / §15.2 系统钩子
// ===========================================================================

/// §8 脚本类别（与 §23 Deliverables 的五个目录一一对应）。
enum class ScriptCategory : std::uint8_t {
    Quest = 0,  ///< scripting/gameplay/quest/   任务规则
    Skill = 1,  ///< scripting/gameplay/skill/   技能公式
    Ai = 2,     ///< scripting/gameplay/ai/      NPC 决策
    Boss = 3,   ///< scripting/gameplay/boss/    Boss 阶段
    Event = 4,  ///< scripting/gameplay/event/   活动/世界事件
};

const char* ToString(ScriptCategory c) noexcept;
/// 解析配置里的类别名；未知返回 false（**禁止静默默认**，§19）。
bool ParseCategory(std::string_view text, ScriptCategory& out) noexcept;

/// §15.2 系统钩子 —— C++ 侧唯一认得的入口分类。
///
/// 「绑定『系统钩子 ↔ 脚本』」= 清单里每条脚本声明自己挂在哪些 hook 的哪些路由键下，
/// C++ 侧只有这五个 `Hook` 常量，没有任何脚本名字面量。
enum class Hook : std::uint8_t {
    SkillFormula = 0,      ///< 技能公式：C++ 传属性 → Lua 算 base+coeff×AP → C++ 结算
    QuestEvent = 1,        ///< 任务事件：C++ 发 MonsterKilled 等 → Lua 更新进度（经命令）
    AiDecide = 2,          ///< NPC 决策：Lua 选动作，C++ 状态机执行
    BossPhase = 3,         ///< Boss 阶段：按 HP% 切阶段/换技能组/召唤/广播
    ActivityModifier = 4,  ///< 活动修正：限时经验/掉落倍率（含自动失效判定）
};

const char* ToString(Hook h) noexcept;
bool ParseHook(std::string_view text, Hook& out) noexcept;

/// 脚本侧可见的 hook 名（`on_event(ctx, name, …)` 的 `name`）。
///
/// 与 `Hook` 分离的原因：hook 名会出现在脚本里，属于**脚本侧契约**，
/// 改名是破坏性变更；`Hook` 枚举只是 C++ 内部路由。两者由本函数单点映射。
std::string_view HookEventName(Hook h) noexcept;

// ===========================================================================
// 清单（config/gameplay/scripts.json）
// ===========================================================================

/// 一条脚本绑定（清单条目）。
struct ScriptBinding {
    std::string name;     ///< 脚本逻辑名（= `ScriptContext::Load` 的名字，如 `skill/fireball`）
    std::string path;     ///< 相对仓库根的源码路径
    ScriptCategory category{ScriptCategory::Quest};
    /// 挂载的 hook（至少一个）。
    std::vector<Hook> hooks;
    /// 路由键：技能 id / 事件名 / AI 档案 / Boss 模板 / 活动 id。
    /// **空 = 通配**（该 hook 的任意键都会命中，按清单顺序短路）。
    std::vector<std::string> keys;
    /// 周期调用频率，仅允许 0（不周期）或 1（1Hz）。> 1 在加载期报错（§21 禁止高频周期脚本）。
    double tick_hz{0.0};
    /// 外部引用声明，形如 `"quest:1001"` / `"boss:dragon"`（`<ns>:<id>`）。
    ///
    /// §19「脚本引用的 Quest / Boss 不存在：加载期校验报错」的执行依据：
    /// 宿主把它交给注入的 `IReferenceRegistry` 逐个校验，任一缺失即加载失败。
    /// 引用**不写死在脚本里**，因此校验可以发生在编译脚本之前（失败时零副作用）。
    std::vector<std::string> requires_refs;
    bool enabled{true};
};

/// 外部引用注册表（§27.4 扩展点：业务侧注入，本模块不依赖 quest/boss 模块）。
///
/// 命名空间约定：`quest` / `skill` / `boss` / `ai` / `activity` / `npc`。
class IReferenceRegistry {
public:
    virtual ~IReferenceRegistry() = default;
    /// 引用是否存在。未知命名空间返回 false（**禁止静默放行**）。
    virtual bool Exists(std::string_view ns, std::string_view id) const noexcept = 0;
};

/// 清单（解析 `config/gameplay/scripts.json` 的产物）。
struct ScriptManifest {
    std::uint32_t version{1};
    std::vector<ScriptBinding> scripts;

    /// 全部处于 enabled 的条目数。
    std::size_t EnabledCount() const noexcept;
};

// ===========================================================================
// §8 载荷容器（固定容量、零堆分配）
// ===========================================================================

/// 标量字段类型（与 Lua 侧表字段一一对应）。
enum class FieldKind : std::uint8_t { Integer = 0, Number = 1, Bool = 2, String = 3 };

/// 一个标量载荷字段。
struct ScalarField {
    static constexpr std::size_t kNameCap = 24;
    static constexpr std::size_t kTextCap = 64;

    char name[kNameCap]{};
    std::uint8_t name_len{0};
    FieldKind kind{FieldKind::Integer};
    std::int64_t i{0};
    double n{0.0};
    bool b{false};
    char text[kTextCap]{};
    std::uint8_t text_len{0};

    std::string_view Name() const noexcept { return {name, name_len}; }
    std::string_view Text() const noexcept { return {text, text_len}; }
};

/// 一次 hook 调用的载荷：事件名 + ≤ 8 个标量字段。
///
/// 为什么是「名字 + 标量字段」而不是任意嵌套表：绑定面刻意收敛（TASK-031 INTERFACE §3），
/// 脚本拿不到任意结构 → 无法凭构造表操纵上层语义。固定容量保证热路径零分配。
class GameplayPayload {
public:
    static constexpr std::size_t kNameCap = 48;
    static constexpr std::size_t kMaxFields = 12;

    void Reset() noexcept;
    /// 事件名（= 脚本 `on_event` 的第 2 个参数）；超长返回 false（**禁止截断**）。
    bool SetName(std::string_view v) noexcept;
    std::string_view Name() const noexcept { return {name_, name_len_}; }

    bool AddInt(std::string_view key, std::int64_t v) noexcept;
    bool AddNum(std::string_view key, double v) noexcept;
    bool AddBool(std::string_view key, bool v) noexcept;
    bool AddStr(std::string_view key, std::string_view v) noexcept;

    std::size_t FieldCount() const noexcept { return field_count_; }
    /// 越界返回 nullptr（禁止越界读）。
    const ScalarField* Field(std::size_t i) const noexcept;

    /// 取整数字段；类型不符或缺失返回 `dflt`。
    std::int64_t GetInt(std::string_view key, std::int64_t dflt = 0) const noexcept;
    /// 取数值字段；Integer 无损提升。
    double GetNum(std::string_view key, double dflt = 0.0) const noexcept;
    bool GetBool(std::string_view key, bool dflt = false) const noexcept;
    std::string_view GetStr(std::string_view key, std::string_view dflt = {}) const noexcept;

private:
    bool Add(ScalarField&& f, std::string_view key) noexcept;

    char name_[kNameCap]{};
    std::uint8_t name_len_{0};
    ScalarField fields_[kMaxFields]{};
    std::size_t field_count_{0};
};

// ===========================================================================
// 五个 hook 的请求 / 结果（纯值；调用方构造请求、消费结果）
// ===========================================================================

/// §22 「单次技能公式调用 < 3us」的输入。
///
/// **C++ / Lua 分工红线**（§7）：本结构体只承载**属性与配置**，
/// 不承载随机数、不承载 AOI 结果、不承载位置积分 —— 那些必须留在 C++。
struct SkillFormulaRequest {
    std::string_view skill_id;      ///< 路由键（= 清单 keys 之一）
    std::int64_t caster{0};
    std::int64_t target{0};
    double base{0.0};               ///< 配置基础值
    double coefficient{0.0};        ///< 属性系数
    double attack_power{0.0};       ///< 施法者攻击强度
    double target_defense{0.0};     ///< 目标防御（仅作公式输入，减免仍由 C++ 结算）
    double target_hp_pct{0.0};      ///< 目标 HP 百分比（0..100）
    std::int64_t caster_level{1};
    /// 0 = 伤害，1 = 治疗。Lua 只算数值，**结算顺序与随机构成仍归 C++**（§7）。
    std::int64_t kind{0};
};

/// 公式计算结果。**永远由 C++ 校验/钳制后才可交给结算**（§19）。
struct SkillFormulaResult {
    bool matched{false};    ///< 清单里有脚本认领该 skill_id
    bool ok{false};         ///< 脚本产出了合法数值
    bool clamped{false};    ///< 发生过钳制（脚本返回 NaN / 负数 / 超上限）
    double raw{0.0};        ///< 脚本原始返回值（诊断用，可能非法）
    double amount{0.0};     ///< 钳制后的数值（交给 C++ 结算）
    const char* reason{nullptr};  ///< `ok == false` 或 `clamped` 时的原因短语
    std::string_view script;      ///< 命中的脚本名
};

struct AiContext {
    std::string_view profile;   ///< 路由键（AI 档案名）
    std::int64_t self{0};
    std::int64_t target{0};
    double hp_pct{100.0};
    double distance{0.0};
    std::int64_t state{0};      ///< C++ 状态机当前状态（Lua 只做决策，不改状态机）
};

/// NPC 动作枚举（与 C++ 状态机对齐；Lua 只能在此范围内选，越界被钳制）。
enum class AiAction : std::int64_t {
    Idle = 0,
    Patrol = 1,
    Chase = 2,
    Attack = 3,
    Flee = 4,
    Return = 5,
    kCount = 6,
};

struct AiDecision {
    bool matched{false};
    bool ok{false};
    AiAction action{AiAction::Idle};
    std::int64_t target{0};
    bool clamped{false};
    std::string_view script;
};

struct BossContext {
    std::string_view boss_template;  ///< 路由键（Boss 模板名）
    std::int64_t boss{0};
    double hp_pct{100.0};
    std::int64_t phase{1};
    std::int64_t enraged{0};
};

/// Boss 阶段决策。C++ 只在 `switched == true` 时应用（避免每 Tick 重复切阶段）。
struct BossPhaseDecision {
    bool matched{false};
    bool ok{false};
    std::int64_t phase{1};
    bool switched{false};
    std::int64_t skill_group{-1};   ///< -1 = 不变更
    std::int64_t summon_npc{0};     ///< 0 = 不召唤
    bool broadcast{false};          ///< 是否要求全员广播（内容由 C++ 组装）
    bool clamped{false};
    std::string_view script;
};

struct ActivityQuery {
    std::string_view activity_id;  ///< 路由键（活动 id）
    std::int64_t player{0};
    std::int64_t now_ms{0};        ///< 墙钟由 C++ 注入（脚本不得自己读时间，§25 确定性）
    std::int64_t exp_gain{0};      ///< 待修正的经验值
};

struct ActivityModifier {
    bool matched{false};
    bool active{false};            ///< 活动是否生效（含限时窗口判定 → 结束自动失效）
    double exp_multiplier{1.0};
    double drop_multiplier{1.0};
    std::int64_t expires_at_ms{0};
    bool clamped{false};
    std::string_view script;
};

// ===========================================================================
// 钳制策略（§19「脚本返回非法值：C++ 侧校验并钳制」）
// ===========================================================================

/// 技能公式数值上限（与 TASK-022 `DamageFormula::max_raw_damage` 同量级）。
inline constexpr double kMaxFormulaAmount = 1'000'000.0;
/// 倍率上限（防脚本写出 1e9 倍经验）。
inline constexpr double kMaxMultiplier = 100.0;
/// Boss 阶段号上限。
inline constexpr std::int64_t kMaxBossPhase = 8;

/// 把脚本返回值钳到 `[0, kMaxFormulaAmount]`。
/// 返回 false 表示发生了钳制（NaN / Inf / 负数 / 超上限）。`reason` 写入原因短语。
bool ClampFormulaAmount(double raw, double& out, const char*& reason) noexcept;

/// 把倍率钳到 `[0, kMaxMultiplier]`（NaN/Inf 视为 0，返回 false 表示钳制过）。
bool ClampMultiplier(double raw, double& out) noexcept;

/// 把 AiAction 钳到合法枚举范围。
AiAction ClampAiAction(std::int64_t raw, bool& clamped) noexcept;

/// 把 Boss 阶段号钳到 `[1, kMaxBossPhase]`。
std::int64_t ClampBossPhase(std::int64_t raw, bool& clamped) noexcept;

// ===========================================================================
// 观测
// ===========================================================================

/// 加载统计（§20 #1「五类脚本各 ≥ 3 个」的可审计依据）。
struct LoadStats {
    std::size_t total{0};
    std::size_t enabled{0};
    std::size_t loaded{0};      ///< 实际装载成功数
    std::size_t failed{0};      ///< 装载失败数（语法错 / 文件缺失）
    std::size_t by_category[5]{};
    std::size_t hooks{0};       ///< 注册的 (hook, key) 路由数
};

}  // namespace mmo::gameplay
