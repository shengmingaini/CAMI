-- scripting/gameplay/quest/escort_merchant.lua
-- TASK-033 · Quest Script —— 限时护送（周期倒计时 + 超时自动失效）
--
-- 演示 §15.3 的「限时任务」：倒计时由 on_tick(dt) 推进（低频 1Hz，禁止高频）。
-- 时间来源是宿主注入的 dt（单调 Clock），脚本**不读墙钟** —— 保证 §25 确定性与可重放。
--
-- 分工红线（§7）：超时判定是「规则」（Lua）；超时后的任务状态处置仍由 QuestSystem 执行，
-- 本脚本的做法是「停止提交进度意图」，从而让该任务不可能被完成。

local ESCORT_QUEST = 1004        -- ReachLocation 3001 ×1
local OBJECTIVE_INDEX = 0
local TALK_NPC = 2002            -- 发布护送任务的 NPC
local TARGET_ZONE = 3001         -- 护送终点

local TIME_LIMIT_MS = 60000      -- 限时 60 秒（Lua 侧规则，可热更）

-- 跨热更保留的状态
escort_active = escort_active or false
escort_left_ms = escort_left_ms or 0
escort_expired = escort_expired or false
escort_done = escort_done or false

local function start_escort()
  escort_active = true
  escort_left_ms = TIME_LIMIT_MS
  escort_expired = false
  escort_done = false
end

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end

  if name == "npc_talked" then
    if payload.npc_id ~= TALK_NPC then return end
    start_escort()
    return
  end

  if name == "location_reached" then
    if payload.zone_id ~= TARGET_ZONE then return end
    if not escort_active or escort_done then return end
    if escort_expired then
      -- 已超时：明确不提交任何进度（任务不可能被完成）
      return
    end
    escort_done = true
    escort_active = false
    quest.set_progress(payload.player, OBJECTIVE_INDEX, 1)
    quest.complete(payload.player, ESCORT_QUEST)
    return
  end
end

function on_tick(ctx, dt)
  if not escort_active then return end
  local step = (dt or 0) * 1000
  escort_left_ms = escort_left_ms - step
  if escort_left_ms <= 0 then
    escort_left_ms = 0
    escort_expired = true
    escort_active = false
  end
end

function on_reload(ctx, old_version)
  -- 热更时必须保留倒计时：护送不能因为换了一版公式就被无偿重置。
  if old_version < 1 then
    escort_active = false
    escort_left_ms = 0
    escort_expired = false
    escort_done = false
  end
end
