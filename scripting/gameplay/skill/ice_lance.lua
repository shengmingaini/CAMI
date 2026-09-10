-- scripting/gameplay/skill/ice_lance.lua
-- TASK-033 · Skill Formula —— 冰锥术（穿透：目标防御越高，公式收益越低）
--
-- §7 契约：结果经 gameplay.result{amount = …} 回传 C++ 结算。
-- 分工红线：减免的**最终**计算仍归 C++（TASK-022 `mitigation_k`），
--   本脚本只是把「防御」作为公式输入之一做软性衰减，两者不冲突也不重复。

local SKILL_ID = "ice_lance"
local BASE = 25.0
local COEFF = 1.05
local DEF_DECAY = 0.0015     -- 每点防御带来的收益衰减
local MIN_FACTOR = 0.35      -- 衰减下限（保证不会算成 0）

formula_calls = formula_calls or 0

function on_init(ctx)
end

function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if type(payload) ~= "table" then return end
  if name ~= "skill_formula" then return end
  if payload.skill ~= SKILL_ID then return end

  formula_calls = formula_calls + 1

  local ap = payload.ap or 0.0
  local defense = payload.defense or 0.0
  local factor = 1.0 - DEF_DECAY * defense
  if factor < MIN_FACTOR then
    factor = MIN_FACTOR
  end

  gameplay.result({ amount = (BASE + COEFF * ap) * factor })
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  if old_version < 0 then
    formula_calls = 0
  end
end
