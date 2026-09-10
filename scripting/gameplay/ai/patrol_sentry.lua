-- scripting/gameplay/ai/patrol_sentry.lua
-- TASK-033 · NPC AI —— 巡逻哨兵（大警戒圈 + 脱战归位）
--
-- 分工红线（§7）：Lua 只选动作；寻路 / 位置积分 / AOI 全在 C++。

local PROFILE = "patrol_sentry"

local ATTACK_RANGE = 2.0
local CHASE_RANGE = 12.0
local RETURN_RANGE = 25.0
local FLEE_HP_PCT = 15.0

decisions = decisions or 0

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

  if hp < FLEE_HP_PCT then
    gameplay.result({ action = 4 })                      -- Flee
    return
  end
  if dist > RETURN_RANGE then
    gameplay.result({ action = 5 })                      -- Return（回岗）
    return
  end
  if target ~= 0 and dist <= ATTACK_RANGE then
    gameplay.result({ action = 3, target = target })
    return
  end
  if target ~= 0 and dist <= CHASE_RANGE then
    gameplay.result({ action = 2, target = target })
    return
  end
  gameplay.result({ action = 1 })                        -- Patrol
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    decisions = 0
  end
end
