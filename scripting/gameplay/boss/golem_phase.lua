-- scripting/gameplay/boss/golem_phase.lua
-- TASK-033 · Boss Phase —— 石魔二阶段（低血硬狂暴）
--
-- 分工红线（§7 / §15.6）：Lua 只判定阶段；狂暴的实际属性加成、广播、技能组切换由 C++ 执行。

local TEMPLATE = "golem"

local ENRAGE_THRESHOLD = 50.0
local HARD_ENRAGE_THRESHOLD = 10.0

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

  if hp_pct <= HARD_ENRAGE_THRESHOLD and current < 3 then
    switch_count = switch_count + 1
    gameplay.result({
      phase = 3,
      switched = true,
      skill_group = 3,
      summon = 0,
      broadcast = true,
    })
    return
  end

  if hp_pct <= ENRAGE_THRESHOLD and current < 2 then
    switch_count = switch_count + 1
    gameplay.result({
      phase = 2,
      switched = true,
      skill_group = 2,
      summon = 0,
      broadcast = true,
    })
  end
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    switch_count = 0
  end
end
