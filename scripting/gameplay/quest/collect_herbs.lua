-- scripting/gameplay/quest/collect_herbs.lua
-- TASK-033 · Quest Script —— 采集草药（多目标 + 品质加权）
--
-- 演示 Lua 擅长的「规则」：同一类事件按物品分级给不同权重，并驱动两个不同任务的目标。
-- 分工红线（§7）：脚本只产出「权重与进度意图」，物品实际扣除 / 进度落库由 C++ 侧状态 Owner 执行。

local HERB_QUEST = 1002          -- CollectItem 9001 ×5
local RARE_HERB_QUEST = 1005     -- CollectItem 9003 ×2
local OBJECTIVE_INDEX = 0

local COMMON_HERBS = { [9001] = true, [9002] = true }
local RARE_HERBS = { [9003] = true }

-- 品质权重（Lua 侧规则，配置在脚本里；数值口径变化走热更，不动 C++）
local WEIGHT = {
  [9001] = 1,   -- 普通草药
  [9002] = 1,   -- 干皮革（复用采集事件）
  [9003] = 2,   -- 稀有草药：一个顶两个
}

-- 跨热更保留的累积状态
herbs_collected = herbs_collected or 0
rare_collected = rare_collected or 0

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "item_collected" then return end

  local item = payload.item_id
  local count = payload.count or 1
  local weight = WEIGHT[item] or 0
  if weight == 0 then return end

  local player = payload.player
  if COMMON_HERBS[item] then
    herbs_collected = herbs_collected + count
    quest.set_progress(player, OBJECTIVE_INDEX, count * weight)
  end
  if RARE_HERBS[item] then
    rare_collected = rare_collected + count
    quest.set_progress(player, OBJECTIVE_INDEX, count * weight)
  end
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 1 then
    herbs_collected = 0
    rare_collected = 0
  end
end
