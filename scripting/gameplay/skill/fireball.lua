-- scripting/gameplay/skill/fireball.lua
-- TASK-033 · Skill Formula —— 火球术（base + coeff × AP，附带斩杀加成）
--
-- §7 契约：四入口，结果经 gameplay.result{amount = …} 回传 C++。
--
-- 分工红线（§7 / §21）：Lua **只算数值**。
--   命中判定、暴击/闪避随机、抗性减免、护盾吸收、扣 HP、致死判定与事件发布
--   全部由 C++（TASK-021/022）完成 —— 本文件里没有任何随机数、没有位置、没有 AOI。

local SKILL_ID = "fireball"
local BASE = 40.0            -- 基础值（技能配置的 Lua 侧镜像；改口径走热更）
local COEFF = 1.35           -- 属性系数
local EXECUTE_BONUS = 1.5    -- 目标血量偏低时的终结加成
local EXECUTE_HP_PCT = 30.0

-- 跨热更保留的累积状态（诊断用：证明热更不丢脚本状态）
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
  local amount = BASE + COEFF * ap
  if (payload.hp_pct or 100.0) < EXECUTE_HP_PCT then
    amount = amount * EXECUTE_BONUS
  end

  -- 只回传数值：最终结算与钳制在 C++（宿主还会再校验一次 NaN / 负数 / 超上限）
  gameplay.result({ amount = amount })
end

function on_tick(ctx, dt)
end

function on_reload(ctx, old_version)
  -- v1 → v2 会调整 COEFF 口径；累积计数刻意保留（热更不丢状态）。
  if old_version < 0 then
    formula_calls = 0
  end
end
