-- scripting/gameplay/event/double_exp.lua
-- TASK-033 · Event Script —— 双倍经验（限时活动，到点自动失效）
--
-- §7 契约：结果经 gameplay.result{active=…, exp_mult=…, drop_mult=…, expires_at=…} 回传。
--
-- 分工红线（§7 / §25）：
--   时间**由 C++ 注入**（payload.now_ms），脚本不读墙钟、不用随机 → 判定可重放；
--   经验/掉落的实际数值结算仍在 C++，脚本只给倍率。

local ACTIVITY_ID = "double_exp"

local WINDOW_START_MS = 1000000      -- 活动窗口起点（宿主注入的毫秒基准）
local DURATION_MS = 3600000          -- 持续 1 小时
local EXP_MULT = 2.0
local DROP_MULT = 1.0

queries = queries or 0
expired_hits = expired_hits or 0

local function in_window(now_ms)
  return now_ms >= WINDOW_START_MS and now_ms < (WINDOW_START_MS + DURATION_MS)
end

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "activity_modifier" then return end
  if payload.activity ~= ACTIVITY_ID then return end

  queries = queries + 1
  local now_ms = payload.now_ms or 0

  if not in_window(now_ms) then
    -- 活动结束：回传 active=false（这就是「自动失效」，不需要外部定时任务来关它）
    expired_hits = expired_hits + 1
    gameplay.result({
      active = false,
      exp_mult = 1.0,
      drop_mult = 1.0,
      expires_at = WINDOW_START_MS + DURATION_MS,
    })
    return
  end

  gameplay.result({
    active = true,
    exp_mult = EXP_MULT,
    drop_mult = DROP_MULT,
    expires_at = WINDOW_START_MS + DURATION_MS,
  })
end

function on_tick(ctx, dt)
  -- 周期回调（1Hz）：本活动用「查询时判定」即可，无需周期工作量。
  -- 保留空实现以演示：周期脚本必须显式声明入口，且频率受清单 tick_hz 约束（≤ 1Hz）。
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    queries = 0
    expired_hits = 0
  end
end
