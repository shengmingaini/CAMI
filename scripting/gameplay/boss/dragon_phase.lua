-- scripting/gameplay/boss/dragon_phase.lua
-- TASK-033 · Boss Phase —— 巨龙四阶段（换技能组 / 召唤 / 阶段播报）
--
-- §7 契约：结果经 gameplay.result{phase=…, switched=…, skill_group=…, summon=…, broadcast=…} 回传。
-- 分工红线（§7 / §15.6）：Lua 只给「阶段判定 + 动作意图」。
--   广播的实际下发、小怪的真实生成、技能组的实际切换都由 C++ 执行（脚本改不了世界状态）。

local TEMPLATE = "dragon"

-- 阶段阈值（血量百分比，从高到低匹配）
local PHASES = {
  { threshold = 70.0, phase = 2, skill_group = 1, summon = 0,    broadcast = true  },
  { threshold = 40.0, phase = 3, skill_group = 2, summon = 3001, broadcast = true  },
  { threshold = 15.0, phase = 4, skill_group = 3, summon = 3002, broadcast = true  },
}

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
  local summon = 0
  local broadcast = false

  for _, p in ipairs(PHASES) do
    if hp_pct <= p.threshold and p.phase > want then
      want = p.phase
      group = p.skill_group
      summon = p.summon
      broadcast = p.broadcast
    end
  end

  if want ~= current then
    switch_count = switch_count + 1
    gameplay.result({
      phase = want,
      switched = true,
      skill_group = group,
      summon = summon,
      broadcast = broadcast,
    })
    return
  end

  -- 不切阶段时不回传结果：避免 C++ 每 Tick 都收到一次「无变化」的动作请求
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  -- 阶段判定是纯函数，无状态需要迁移；仅诊断计数保留。
  if old_version < 0 then
    switch_count = 0
  end
end
