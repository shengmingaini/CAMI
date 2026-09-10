-- scripting/gameplay/event/festival_lantern.lua
-- TASK-033 · Event Script —— 灯节庆典（双倍掉落 + 经验加成，到点自动失效）
--
-- §7 契约：结果经 gameplay.result{...} 回传；时间由 C++ 注入（payload.now_ms）。
--
-- 分工红线（§7 / §21）：Lua 只给倍率，**不给绝对数值**；
--   经验/掉落的最终结算、活动纪念品的发放都走 C++ 系统接口。

local ACTIVITY_ID = "festival_lantern"

local WINDOW_START_MS = 3000000
local DURATION_MS = 7200000          -- 2 小时
local EXP_MULT = 1.5
local DROP_MULT = 2.0
-- 关闭前最后 10 分钟：加成衰减（Lua 侧的「活动节奏」规则，改它只需热更本文件）
local SOFT_CLOSE_MS = 600000
local SOFT_EXP_MULT = 1.2

queries = queries or 0

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

  if now_ms >= (ends_at - SOFT_CLOSE_MS) then
    gameplay.result({
      active = true,
      exp_mult = SOFT_EXP_MULT,
      drop_mult = DROP_MULT,
      expires_at = ends_at,
    })
    return
  end

  gameplay.result({ active = true, exp_mult = EXP_MULT, drop_mult = DROP_MULT, expires_at = ends_at })
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    queries = 0
  end
end
