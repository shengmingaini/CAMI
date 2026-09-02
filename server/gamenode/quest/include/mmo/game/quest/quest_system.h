#pragma once

/// TASK-019 · QuestSystem 公开接口（§7，冻结契约）。
///
/// 命名空间：mmo::game::quest。事件驱动任务系统：Accept / Abandon / TurnIn / OnEvent。
///
/// 依赖（§27.2，仅消费公开接口）：
///   TASK-007 engine/core（EventBus / Result / SteadyTime / TraceID）
///   TASK-016 role（玩家等级查询，经 IPlayerQuery 抽象，Quest 不持有等级副本）
///   TASK-018 ai（击杀事件源，经本模块 quest_events.h 的 MonsterKilled 契约）
///
/// 红线（§21）：禁止每 Tick / 每秒遍历所有玩家检查所有任务；进度更新只由事件触发；
///             禁止同步数据库访问；奖励必须幂等。

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/quest/quest_def.h"
#include "mmo/game/quest/quest_index.h"
#include "mmo/game/quest/quest_instance.h"

namespace mmo::game::quest {

/// 玩家只读查询（消费 TASK-016 Role）。
/// Quest 不持有等级副本 —— 等级属 Role 的实时状态（§4 State Owner），此处只经接口读。
class IPlayerQuery {
public:
    virtual ~IPlayerQuery() = default;
    /// 未知玩家返回 0（会因等级不足被拒）。
    virtual std::uint32_t LevelOf(PlayerId player) const noexcept = 0;
};

class QuestSystem {
public:
    /// 事件驱动入口的统一信封（24B）。
    ///
    /// 契约偏差说明：任务书 §7 写作 `OnEvent(const core::EventEnvelope&)`，但上游 core
    /// 不存在 `EventEnvelope`（EventBus 为强类型 `Publish<TEvent>`，见 TASK-007 §7）。
    /// 故本模块导出等价信封，语义完全一致：一次调用携带「类型 + 目标 + 数量 + 玩家」。
    struct Event {
        ObjectiveType type{ObjectiveType::KillMonster};
        std::uint32_t target_id{0};
        std::uint32_t count{1};
        PlayerId player{0};
    };

    /// 奖励发放接口（TASK-029 Economy 未就绪前的**幂等占位接口**，签名与最终一致）。
    /// 发放失败时 QuestSystem 不标记 TurnedIn，可安全重试（§19）。
    class IRewardSink {
    public:
        virtual ~IRewardSink() = default;
        virtual core::Result<void> Grant(PlayerId player, const QuestDef& def,
                                         core::TraceID trace) = 0;
    };

    /// bus 可为空（纯 API 模式）；非空时用于发布 QuestCompleted。
    explicit QuestSystem(core::EventBus* bus = nullptr) noexcept;

    // ---- 配置化加载（§21 禁止硬编码任务配置） ----

    /// 从目录加载任务定义，返回加载条数。缺字段/引用非法一律报错，禁止静默默认（§19）。
    core::Result<std::size_t> LoadQuests(std::string_view dir);

    /// 注册（或覆盖）一条任务定义：供配置热更与测试注入使用（§13）。
    /// 禁止在事件热路径频繁调用；已持有该任务的玩家不受影响（定义只被读取）。
    core::Result<void> RegisterDef(QuestDef def);

    const QuestDef* FindDef(QuestId id) const noexcept;
    std::size_t DefCount() const noexcept { return defs_.size(); }

    // ---- 生命周期（§7） ----

    /// 接受任务：校验等级与前置任务；失败返回明确错误码（§15.5）。
    core::Result<void> Accept(PlayerId player, QuestId quest, core::TraceID trace);

    /// 放弃任务：清空进度并退出索引（可重新接受）。
    core::Result<void> Abandon(PlayerId player, QuestId quest, core::TraceID trace);

    /// 交还任务：**幂等**（player+quest 去重），重复请求只发一次奖励（§19 / §20.4）。
    /// 奖励发放失败时保持 Completed，可重试。
    core::Result<void> TurnIn(PlayerId player, QuestId quest, core::TraceID trace);

    // ---- 事件驱动入口（唯一的进度写入口，§8） ----

    /// 处理一条事件：倒排索引 O(1) 定位 → 只更新命中的任务目标。
    /// 未命中任何目标时 O(1) 返回，不触碰任何玩家数据。
    core::Result<void> OnEvent(const Event& ev);

    // ---- 观测（§7 / §16） ----

    const QuestInstance* Find(PlayerId player, QuestId quest) const noexcept;
    /// 进行中（Accepted 或 Completed）的任务数。
    std::size_t ActiveQuests(PlayerId player) const noexcept;
    /// 已注册的事件处理器数（指标）。
    std::size_t EventHandlerCount() const noexcept { return handler_count_; }
    std::size_t PlayerCount() const noexcept { return players_.size(); }
    /// 奖励实际发放次数（幂等验证：重复交任务后应保持不变）。
    std::size_t RewardGrantCount() const noexcept { return grant_count_; }
    std::size_t IndexEntryCount() const noexcept { return index_.EntryCount(); }
    std::size_t IndexBucketCount() const noexcept { return index_.BucketCount(); }

    // ---- 依赖注入（均为非拥有引用） ----

    /// 订阅四类事件 → 转 OnEvent；同时用于发布 QuestCompleted（消费 TASK-007 公开接口）。
    core::Result<void> BindEventBus(core::EventBus& bus);
    void BindPlayerQuery(IPlayerQuery& q) noexcept { player_query_ = &q; }
    void BindRewardSink(IRewardSink& sink) noexcept { reward_sink_ = &sink; }

    /// 已处理事件计数（测试 / 基准用）。
    std::size_t EventCount() const noexcept { return event_count_; }

private:
    core::Result<void> CanAccept(PlayerId player, const QuestDef& def) const;
    void InsertObjectives(PlayerId player, QuestId quest, const QuestDef& def);
    void RefreshCompletion(PlayerId player, QuestId quest, QuestInstance& inst,
                           const QuestDef& def);
    static std::uint64_t IdemKey(PlayerId player, QuestId quest) noexcept {
        // 避免 (player<<32)|quest 在 player ≥ 2^32 时碰撞
        return player ^ (static_cast<std::uint64_t>(quest) * 0x9E3779B97F4A7C15ull);
    }
    static core::Result<void> Fail(core::ErrorCode code, const char* what);

    core::EventBus* bus_{nullptr};
    IPlayerQuery* player_query_{nullptr};
    IRewardSink* reward_sink_{nullptr};

    std::unordered_map<QuestId, QuestDef> defs_;  // 进程级配置，不占玩家内存（§22）
    /// 玩家 → (任务 id → 实例)
    std::unordered_map<PlayerId, std::unordered_map<QuestId, QuestInstance>> players_;
    QuestIndex index_;
    std::unordered_set<std::uint64_t> turned_in_;  // 幂等表：IdemKey(player, quest)

    /// 事件条目草稿：回调期间会修改索引，先拷出再遍历（避免迭代中失效）。
    std::vector<QuestIndex::Entry> scratch_;

    std::size_t handler_count_{0};
    std::size_t grant_count_{0};
    std::size_t event_count_{0};
};

}  // namespace mmo::game::quest
