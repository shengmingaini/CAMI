-- scripting/gameplay/event/world_boss_dawn.lua
-- TASK-033 · Event Script —— 世界 BOSS 降临（限时：经验小加成 + 掉落加成）
--
-- §7 契约：结果经 gameplay.result{...} 回传；时间由 C++ 注入（payload.now_ms）。
--
-- 分工红线（§7）：世界事件的实际调度（Boss 生成、全服通知）由 C++ 控制面执行；
--   本脚本只回答「此刻该活动是否生效、倍率多少」。

local ACTIVITY_ID = "world_boss_dawn"

local WINDOW_START_MS = 2000000
local DURATION_MS = 1800000          -- 30 分钟
local EXP_MULT = 1.2
local DROP_MULT = 1.5

queries = queries or 0
active_hits = active_hits or 0

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "activity_modifier" then return end
  if payload.activity ~= ACTIVITY_ID then return end

  queries = queries + 1
  local now_ms = payload.now_ms or 0
  local ends_at = WINDOW_START_MS + DURATION_MS

  if now_ms < WINDOW_START_MS or now_ms >= ends_at then
    gameplay.result({ active = false, exp_mult = 1.0, drop_mult = 1.0, expires_at = ends_at })
    return
  end

  active_hits = active_hits + 1
  gameplay.result({ active = true, exp_mult = EXP_MULT, drop_mult = DROP_MULT, expires_at = ends_at })
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    queries = 0
    active_hits = 0
  end
end
