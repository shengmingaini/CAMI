-- scripting/gameplay/quest/kill_10_wolves.lua
-- TASK-033 · Quest Script —— 狼群清剿（监听击杀事件更新进度 + 交还发奖）
--
-- §7 契约（C++ 只认这四个函数）
--   on_init(ctx)                       脚本装载
--   on_event(ctx, name, payload)       事件回调（payload 为表）
--   on_tick(ctx, dt)                   可选周期（≤ 1Hz，本脚本不需要）
--   on_reload(ctx, old_version)        热更后的状态迁移
--   ctx     = 宿主句柄字符串（"scene/<id>"）；结构化上下文用 gameplay.ctx() 取表
--   payload = 宿主经 gameplay.payload() 投递；Lua 侧单测可直接传入 mock 表
--
-- 分工红线（§7）：本脚本只做「规则判定 + 进度意图」。
--   进度的真正写入者是 QuestSystem（经 quest.set_progress 命令 → 状态 Owner 执行）；
--   奖励由 QuestSystem 经 IRewardSink 发放，脚本既看不到也改不了数值。

local QUEST_ID = 1001          -- config/gameplay/quests/quests.json
local OBJECTIVE_INDEX = 0      -- 该任务的第 0 个目标（KillMonster）
local WOLF_FAMILY = { [1001] = true, [1002] = true }   -- 普通狼 / 头狼

-- 跨热更保留的累积状态：**全局 + or 初值**，不能用 local。
-- local 是闭包 upvalue，ReloadInPlace 后新闭包会整体替换它 → 计数丢失（docs/README.md R3）。
wolves_seen = wolves_seen or 0
turned_in = turned_in or false

local function is_wolf(monster_id)
  return monster_id ~= nil and WOLF_FAMILY[monster_id] == true
end

function on_init(ctx)
  -- 首次装载才会走到这里；热更只走 on_reload，故此处不重置累积量
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end

  if name == "monster_killed" then
    if not is_wolf(payload.monster_id) then return end
    local count = payload.count or 1
    wolves_seen = wolves_seen + count
    -- 只提交「意图」：真正的进度写入由 QuestSystem（状态 Owner）完成
    quest.set_progress(payload.player, OBJECTIVE_INDEX, count)
    return
  end

  if name == "quest_completed" then
    if payload.quest ~= QUEST_ID or turned_in then return end
    turned_in = true
    -- 交还由 QuestSystem 执行并幂等发奖（重复调用只发一次，TASK-019 §19）
    quest.complete(payload.player, QUEST_ID)
    return
  end
end

function on_tick(ctx, dt)
  -- 无周期需求：显式空实现（契约要求四入口齐全，装载期探针会校验）
end

function on_reload(ctx, old_version)
  -- 状态迁移：只有口径变化才动状态；累积量刻意保留 —— 热更期间不丢计数。
  if old_version < 1 then
    wolves_seen = 0
    turned_in = false
  end
end
