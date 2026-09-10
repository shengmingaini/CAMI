-- scripting/gameplay/boss/lich_phase.lua
-- TASK-033 · Boss Phase —— 巫妖三阶段（召唤骷髅 + 阶段播报）
--
-- 分工红线（§7 / §15.6）：Lua 只判定阶段与动作意图；C++ 执行广播与召唤。

local TEMPLATE = "lich"

local PHASES = {
  { threshold = 60.0, phase = 2, skill_group = 1, summon = 3101, broadcast = true },
  { threshold = 30.0, phase = 3, skill_group = 2, summon = 3102, broadcast = true },
}

-- 巫妖的特点：阶段越高，召唤越多（用「每次切阶段多召一只」表达）
local SUMMON_STEP = 1

switch_count = switch_count or 0

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "boss_phase" then return end
  if payload.boss ~= TEMPLATE then return end

  local hp_pct = payload.hp_pct or 100.0
  local current = payload.phase or 1

  local want = current
  local group = -1
  local npc = 0
  local broadcast = false

  for _, p in ipairs(PHASES) do
    if hp_pct <= p.threshold and p.phase > want then
      want = p.phase
      group = p.skill_group
      npc = p.summon
      broadcast = p.broadcast
    end
  end

  if want == current then return end

  switch_count = switch_count + 1
  local count = SUMMON_STEP * (want - 1)
  gameplay.result({
    phase = want,
    switched = true,
    skill_group = group,
    summon = npc * count,    -- 0 表示不召唤
    broadcast = broadcast,
  })
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    switch_count = 0
  end
end
