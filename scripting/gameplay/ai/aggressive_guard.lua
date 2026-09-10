-- scripting/gameplay/ai/aggressive_guard.lua
-- TASK-033 · NPC AI —— 主动警戒卫兵（巡逻 / 追击 / 攻击 / 逃跑）
--
-- 分工红线（§7 / §15.5）：Lua **只做决策**，返回一个动作枚举；
--   状态机的执行、寻路、位置积分、攻击冷却全部在 C++（TASK-018）。
--   脚本拿不到也不返回任何位置/坐标，只用宿主给的标量（距离、血量百分比）。
--
-- 动作枚举（与 C++ 对齐，越界会被宿主钳制为 Idle）：
--   0 Idle / 1 Patrol / 2 Chase / 3 Attack / 4 Flee / 5 Return

local PROFILE = "aggressive_guard"

local ATTACK_RANGE = 1.8
local CHASE_RANGE = 9.0
local LEASH_RANGE = 28.0
local FLEE_HP_PCT = 20.0

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
  if target == 0 or dist > LEASH_RANGE then
    gameplay.result({ action = 1 })                      -- Patrol（脱战归位）
    return
  end
  if dist <= ATTACK_RANGE then
    gameplay.result({ action = 3, target = target })     -- Attack
    return
  end
  if dist <= CHASE_RANGE then
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
  end
end
