-- scripting/gameplay/skill/heal_light.lua
-- TASK-033 · Skill Formula —— 微光治疗（治疗公式：随施法者等级成长并封顶）
--
-- §7 契约：结果经 gameplay.result{amount = …} 回传 C++。
-- 分工红线：实际回血、过量治疗统计、治疗事件发布由 C++（TASK-022）完成。

local SKILL_ID = "heal_light"
local BASE = 30.0
local COEFF = 1.10
local LEVEL_BONUS = 2.5      -- 每级追加
local LEVEL_GROWTH_CAP = 60  -- 等级收益封顶（防高等级线性膨胀）

formula_calls = formula_calls or 0

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "skill_formula" then return end
  if payload.skill ~= SKILL_ID then return end

  formula_calls = formula_calls + 1

  local level = payload.level or 1
  local capped = level
  if capped > LEVEL_GROWTH_CAP then
    capped = LEVEL_GROWTH_CAP
  end

  gameplay.result({ amount = BASE + COEFF * (payload.ap or 0.0) + LEVEL_BONUS * capped })
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    formula_calls = 0
  end
end
