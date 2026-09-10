-- scripting/gameplay/ai/fleeing_scout.lua
-- TASK-033 · NPC AI —— 怯战斥候（半血即撤，残血必逃）
--
-- 分工红线（§7）：Lua 只选动作；C++ 状态机负责执行与「撤退点」的寻路。

local PROFILE = "fleeing_scout"

local PANIC_HP_PCT = 50.0    -- 半血开始撤退
local HOLD_HP_PCT = 80.0     -- 高血量才敢反击
local HOLD_RANGE = 5.0
local CHASE_RANGE = 18.0

decisions = decisions or 0
flees = flees or 0

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "ai_decide" then return end
  if payload.profile ~= PROFILE then return end

  decisions = decisions + 1

  local hp = payload.hp_pct or 100.0
  local dist = payload.distance or 0.0
  local target = payload.target or 0

  if hp < PANIC_HP_PCT then
    flees = flees + 1
    gameplay.result({ action = 4 })                      -- Flee
    return
  end
  if hp >= HOLD_HP_PCT and target ~= 0 and dist <= HOLD_RANGE then
    gameplay.result({ action = 3, target = target })     -- Attack
    return
  end
  if target ~= 0 and dist <= CHASE_RANGE then
    gameplay.result({ action = 2, target = target })     -- Chase
    return
  end
  gameplay.result({ action = 1 })                        -- Patrol
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    decisions = 0
    flees = 0
  end
end
