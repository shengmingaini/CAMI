# TASK-033 · Gameplay Script 开发文档

> 模块：`scripting/gameplay`（C++/Lua 桥接层）+ `config/gameplay/scripts.json`（清单）
> 配合：TASK-031（Lua Runtime）/ TASK-032（Lua Hot Reload）/ TASK-018·019·021（各 C++ 系统）
> 本文是「脚本作者」视角：怎么写、怎么配、红线在哪、怎么热更。

---

## 1. 定位与 C++/Lua 分工

`GameplayScriptHost` 是「系统钩子 ↔ 玩法脚本」之间缺失的那段桥。它把 C++ 业务系统的
纯值请求路由到 Lua 脚本，再把脚本回传的数值交给 C++ 做最终裁决（钳制 / 结算 / 状态迁移）。

| 能力 | 归属 | 理由 |
|---|---|---|
| Simulation / Entity / Memory / Scheduler / Network / AOI / 核心战斗框架 | **C++** | 性能与确定性 |
| Quest 规则 / Skill 公式 / NPC AI / Boss 阶段 / 活动脚本 | **Lua** | 迭代频率高 |
| 位置积分、伤害数值结算、AOI 计算 | **C++** | 禁止下放到 Lua |

**五条不可越过的红线**（违反即被拦截 / 钳制 / 回退 C++ 默认）：
1. 脚本从不直接改实时状态——所有写都是一条 `ScriptCommand`，由状态 Owner 执行；
2. 位置积分 / AOI / 伤害最终结算不下放 Lua；
3. 脚本返回值一律经 C++ 校验并钳制（NaN / 负数 / 超上限 → 钳 + 标记 `clamped`）；
4. 禁止脚本做 IO（`io.*` / `os.*` / `loadstring` 在沙箱静态扫描阶段被拒）；
5. 周期脚本 ≤ 1Hz（清单层直接拒绝 `tick_hz > 1`）。

---

## 2. 脚本契约（四入口）

所有玩法脚本遵循统一入口，**C++ 只认这四个函数**：

```lua
function on_init(ctx)          end   -- 脚本加载（仅一次）
function on_event(ctx, name, payload) end   -- 事件回调（核心计算在此）
function on_tick(ctx, dt)      end   -- 可选周期（低频，清单声明 tick_hz）
function on_reload(ctx, old_version) end   -- 热更后的状态迁移（old_version 为真实旧版本号）
```

- `ctx`：只读上下文表，由 `gameplay.ctx()` 在调用内提供（见 §4.1）。
- `payload`：本次事件的载荷表，由 `gameplay.payload()` 提供（见 §4.2）。
- 结果通过 `gameplay.result{…}` 回传（见 §4.3）——**不要**试图 `return` 表，C++ 不收返回值。

> 契约探针：装载期宿主会真实调用 `on_init`、向 `on_event` 发 `__contract_probe`、
> 向 `on_tick` 发 `dt=0`、向 `on_reload` 发 `old_version=0`。脚本在这三种入参下必须**无副作用**。

---

## 3. 清单配置 `config/gameplay/scripts.json`

脚本路径与绑定关系**全部配置化**，C++ 侧没有任何脚本名字面量（§27.3）。

```json
{
  "scripts": [
    {
      "name": "skill/fireball",
      "path": "scripting/gameplay/skill/fireball.lua",
      "category": "skill",
      "hooks": ["skill_formula"],
      "keys": ["fireball"],
      "tick_hz": 0,
      "requires": []
    }
  ]
}
```

字段说明：

| 字段 | 含义 |
|---|---|
| `name` | 逻辑名（路由键，带命名空间如 `skill/fireball`） |
| `path` | 源码相对路径（相对仓库根；宿主 `Config::root` 非空时前置） |
| `category` | `quest` / `skill` / `ai` / `boss` / `event`（与 hook 必须匹配） |
| `hooks` | 认领的钩子：`quest_event` / `skill_formula` / `ai_decide` / `boss_phase` / `activity_modifier` |
| `keys` | 路由键列表；**空数组 = 通配**（任意 key 都路由到该脚本，但脚本自身仍按 payload 决定是否认领） |
| `tick_hz` | 周期频率；必须 ≤ 1，否则装载失败（fail-fast） |
| `requires` | 外部引用校验，形如 `"quest:1001"`；引用不存在 → 装载期报错（零副作用） |

校验规则（装载期 `ValidateManifest`）：类别↔hook 匹配、tick_hz ≤ 1、字段齐全、无重名、
`requires` 形如 `<ns>:<id>`。任何一项不过 → 明确报错，**禁止静默默认**。

---

## 4. `gameplay.*` 绑定 API

脚本在 `on_event` 体内通过以下原生绑定与宿主交换数据。表数据生命周期锁定在**本次同步调用**内，
无跨 Tick 悬挂风险。

### 4.1 `gameplay.ctx()` — 只读上下文表

| 键 | 类型 | 含义 |
|---|---|---|
| `scene` | int | 场景 ID |
| `tick` | int | 当前 Tick 号 |
| `version` | int | 当前脚本版本号 |
| `script` | str | 本脚本逻辑名 |
| `category` | str | 类别 |
| `hook` | str | 当前钩子名（`skill_formula` / `quest_event` / `ai_decide` / `boss_phase` / `activity_modifier`） |

### 4.2 `gameplay.payload()` — 本次事件载荷表

| 键 | 类型 | 含义 |
|---|---|---|
| `event` | str | 事件名（如 `monster_killed` / `monster_killed` 等，由 C++ 经 `GameplayPayload::SetName` 设置） |
| （其余） | int/num/bool/str | C++ 注入的标量字段（如 `skill` / `monster_id` / `hp_pct` / `target` …） |

> 脚本只拿到宿主愿意给的标量表，无法构造任意结构操纵上层语义（协议面刻意收窄）。

### 4.3 `gameplay.result{…}` — 回传结果

C++ 只识别下表中的键（其余忽略）。**按 hook 类型各自取用对应键**：

| Hook | 应回传的键 | 说明 |
|---|---|---|
| `skill_formula` | `amount`（num） | 伤害/治疗公式结果，C++ 再钳制 + 结算 |
| `ai_decide` | `action`（int）、`target`（int 可选） | 动作枚举 + 目标实体；坐标/寻路由 C++ 状态机执行 |
| `boss_phase` | `phase`（int）、`switched`（bool）、`skill_group`（int 可选）、`summon`（int 可选）、`broadcast`（bool 可选） | 切阶段时给出新阶段与附带效果 |
| `activity_modifier` | `active`（bool）、`exp_mult`（num）、`drop_mult`（num）、`expires_at`（int 可选） | 活动倍率；`expires_at` 用于「到点自动失效」 |
| `quest_event` | 通常无（走命令，见 §4.5） | 进度更新走 `quest.set_progress` 命令 |

示例（fireball 公式）：
```lua
function on_event(ctx, name, payload)
  payload = payload or gameplay.payload()
  if name ~= "skill_formula" then return end
  if payload.skill ~= "fireball" then return end          -- 不认领则调用方走 C++ 默认公式
  local ap = payload.attack_power or 0
  local raw = 40.0 + 1.35 * ap
  if payload.target_hp_pct < 30.0 then raw = raw * 1.5 end  -- 斩杀加成
  gameplay.result({ amount = raw })
end
```

### 4.4 `gameplay.hook()` — 当前钩子名

返回字符串（与 `ctx.hook` 同值），脚本侧不必解析 ctx 表即可分支。

### 4.5 受控写：经命令落地的意图

脚本**不能**直接改实时状态。需要改状态时用对应的命令 API，由宿主注册的 op handler
（业务系统的状态 Owner）执行：

- `entity.set_hp(target, value)` — 改 HP（目标实体必须真实存在，否则命令被拒）
- `quest.set_progress(player, quest_id, value)` — 更新任务进度
- `skill.cast(...)` — 触发技能
- `event.*` — 派发事件

> 这些调用在 Lua 侧只是「提交意图 + 参数」，真正的写入发生在 C++ 侧。单测
> `test_redline_controlled_write_only` 固化了「未注入 handler → 调用作废 + 报错；
> 注入后 → 参数原样交给状态 Owner」的行为。

---

## 5. 红线（违反即拦截 / 钳制）

| 越界行为 | 结果 |
|---|---|
| 脚本内写 `io.open` / `os.time` / `loadstring` | 装载期沙箱静态扫描拒绝，该脚本不进路由表 |
| `gameplay.result({ amount = 0/0 })` | C++ 钳到 0，标记 `clamped` |
| `gameplay.result({ amount = -5 })` | 钳到 0 |
| `gameplay.result({ amount = 1e12 })` | 钳到 `kMaxFormulaAmount` |
| `gameplay.result({ action = 99 })` | 钳到 `Idle` |
| `gameplay.result({ phase = 99 })` | 钳到 `kMaxBossPhase` |
| 单条脚本语法错 / 运行期报错 / 超限 | 只作废该次调用，其余脚本与 Scene Tick 照常（§19 失败隔离） |
| 装载期任一脚本失败（`require_all_scripts=true`） | 整体 fail-fast；`false` 时记录并继续，对应 hook 回退 C++ 默认 |

---

## 6. 新增一个脚本的步骤

1. 在 `scripting/gameplay/<category>/` 下新建 `xxx.lua`，实现四入口（至少 `on_event`）。
2. 在 `config/gameplay/scripts.json` 的 `scripts` 数组追加一条：`name` / `path` / `category` /
   `hooks` / `keys`（与 `category` 匹配的 hook；路由键按业务字段定）。
3. 周期脚本才填 `tick_hz`（≤ 1）；引用其他系统实体填 `requires`（`<ns>:<id>`）。
4. 运行 `bash scripts/verify/task-033.sh` 或本地单测验证装载与路由。
5. 不要在任何 C++ 文件里写脚本名——路由全部来自清单（§27.3）。

---

## 7. 热更（Hot Reload）

标准三步（缺 `Validate` 这一步 `Activate` 必返回 `UNAUTHORIZED`）：

```cpp
auto ticket  = host->PrepareReload("skill/fireball", patched_source);  // 隔离 VM 编译
auto report  = host->ValidateReload(ticket);                           // 沙箱静态扫描 + 冒烟执行
host->BeginSafePoint(tick);                                           // 进入 Tick 安全点
host->ActivateReload(ticket, trace);                                  // 原子替换（仅安全点内）
host->EndSafePoint();
```

- 回滚：`host->Rollback("skill/fireball", trace)`（同样仅安全点内）。
- 版本号**单调审计序号**（回滚也占新序号，便于追溯），内容以 checksum 判定。
- 不在清单里的名字一律拒绝热更（`PrepareReload("no/such", ...)` → 失败）。
- 非法源码（如 `io.open`）在 `Validate` 阶段被拦，`Activate` 拒绝，线上版本不变。
- 热更保留脚本全局状态（`on_init` 不重跑，避免破坏状态连续性；`on_reload(old_version)` 负责迁移）。

> ⚠️ 当前 `ActivateReload` 由调用方**手动**在 `BeginSafePoint`/`EndSafePoint` 之间触发
> （单测即此模式）。生产环境应由 SimulationScheduler 在 Tick 边界自动宣告安全点并驱动激活——
> 见 §8 的已知架构缺口。

---

## 8. 已知架构缺口：Tick 安全点（TASK-013 集成）

TASK-033 的热更已能在「逻辑上正确」地工作（Prepare→Validate→Activate→Rollback 全链路通过），
但**生产环境的自动安全点尚未接通**：

- TASK-013 交付的 `SimulationScheduler`（20Hz 固定 Tick，八阶段）其公开接口**没有提供任何
  安全点（Safe Point）钩子**——既没有回调、也没有可供注册的「每个 Tick 的某边界点」。
- 宿主头注释里提到的 `ScriptReloadStage` 实际属于 **TASK-032**（`scripting/lua`），并非 TASK-013
  产出；且 `SimulationScheduler` 从未注册/调用它。
- 因此 `GameplayScriptHost::ActivateReload` 目前只由调用方手动在 `BeginSafePoint`/`EndSafePoint`
  之间触发（单测与演练即此模式），**未被接入真实的 20Hz Tick 循环**。

这是一个**跨模块**的架构决策点（触及 TASK-013 `server/gamenode/scheduler` 与 TASK-032
`ScriptReloadStage`，超出 TASK-033 的 `scripting/gameplay` 子树边界 §27.3），**不应由本任务静默修改**，
需由架构负责人决定落地方式（例如：TASK-013 增加安全点边界 API / 用一个 `ISimulationStage`
包装 `ScriptReloadStage` 在 Tick 内开启安全点 / 由宿主在 Quest/Event 阶段边界驱动）。
详见 `docs/gameplay-script-report.md` §已知缺口。

---

## 9. 排障参考：Lua 5.5 分配器记账（回归已锁死）

历史坑位（已由 TASK-031 修复、TASK-033 单测 `test_allocator_memory_accounting_regression`
锁死）：Lua 5.5 在「新建块」时把**对象 tag（8）**作为 `osize` 传给 `lua_Alloc`
（`lmem.c`：*frealloc(ud, NULL, x, s) 创建新块，大小 's'，'x' 无关*）。若分配器在
`ptr==NULL` 时也减去 `osize`，每次新建块少记 8 字节，而释放按真实尺寸扣减，一进一出让
`mem_used_` 单调下漂并回绕到 ~2⁶⁴，此后任何分配都被拒 → 每条 hook 调用 `MemoryLimit`
失败（表现为 `skill_formula_ns` 飙到 ~7µs 且全部报错）。修复：仅 `ptr!=NULL` 时减旧尺寸。

若将来看到「脚本各处莫名 MemoryLimit / 热路径延迟突然飙升」，先查分配器记账，再查内存上限配置。
