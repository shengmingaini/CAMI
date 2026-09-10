# TASK-031 · Lua Runtime —— 接口说明（INTERFACE）

本文件是 `scripting/lua` 的**对外契约**，分两半读：

- **§1–§6 脚本侧**：Lua 脚本看得见什么、怎么写、拿到的返回值长什么样。
- **§7–§12 宿主侧**：C++ 怎么装配、怎么调用、错误码怎么往返、线程与热路径纪律。

> 权威定义在头文件里（`include/mmo/script/*.h`），本文件只做「怎么用」的说明。
> 两者冲突时以头文件为准，并按 §27 流程修正本文件。

---

## 1. 装配顺序（宿主侧，启动期一次）

```
Create(limits)              // 建 VM + 开沙箱 + 注册内置绑定面（10 条）
  → BindEntityApi(entities)   // 可选，未绑则 entity.* 报 INVALID_ARGUMENT
  → BindEventApi(events)      // 可选
  → BindQueryApi(queries)     // 可选
  → BindCommandApi(commands)  // 可选
  → RegisterCommandOp(op, …)  // 由业务系统声明它能处理哪些 op
  → RegisterQueryOp(op, …)
  → AddBinding(自定义绑定)     // 可选：宿主扩展点（§27.4）
  → FreezeBindings()          // 冻结名册：此后 Add 一律 BUSY
  → Load(name, source) …      // 装载脚本
  → [Tick 循环] Call(id, fn)  // 热路径
  → [Tick Safe Point] ApplyPendingChanges()
```

**生命周期红线**：`entities` / `events` / `queries` / `commands` 必须比 `ScriptContext`
**长寿**。`~ScriptContext` 会向绑定的总线退订自己安装的订阅；总线先析构就是
use-after-free（实测崩溃点 `EventBus::Unsubscribe`）。局部变量按声明**逆序**析构，
所以总线要声明在 `ScriptContext` 之前。

---

## 2. 脚本侧名字空间（共 5 类 + 1 扩展位）

| 脚本里写的 | 绑定类别 | 语义 | 参数个数 |
|---|---|---|---|
| `entity.get(id)` | Entity | 只读直查实体快照 | 1 |
| `entity.get_pos(id)` | Entity | 只读直查位置 | 1 |
| `entity.set_hp(id, v)` | Entity | **受控写** → 走 `CommandBus`（op `entity.set_hp`） | 2 |
| `skill.cast(caster, skill_id, target)` | Skill | → `CommandBus`（op `skill.cast`） | 3（严格） |
| `quest.set_progress(player, idx, v)` | Quest | → `CommandBus`（op `quest.set_progress`） | 3（严格） |
| `quest.complete(player, id)` | Quest | → `CommandBus`（op `quest.complete`） | 2（严格） |
| `event.subscribe(name, fn)` | Event | 注册 Lua 回调（C++ 侧装订阅） | 2 |
| `event.publish(name, ...)` | Event | 发自定义事件 → 真实 `EventBus`（**异步**） | ≥1 |
| `query.ask(op, ...)` | Query | 只读查询 → `QueryBus`（op 取第 1 个参数） | ≥1 |
| 宿主自定义 | Native | 由 `AddBinding` 注册，如测试的 `probe.*` | 自定义 |

**要点**

- **`skill.*` / `quest.*` 不是直连战斗/任务模块**。本模块依赖集不含 combat / quest，
  所以这些调用被表达为 `ScriptCommand{op, args}` 交给统一 `CommandBus`，
  由业务系统注册的 op handler 执行 → 满足「只走系统接口」与「不为脚本再开 RPC 通道」。
- **Entity 只读面直查**（`EntityManager::Find`），不制造额外命令流量。
- **`entity.set_hp` 是唯一的内置写入口**，且它也只是「请求」：真正改 HP 的是
  宿主注册的 op handler。脚本无法直接改实体内部成员。
- `event.publish` **异步**：脚本 publish 后，订阅者在之后某次 `EventBus::Drain` 才收到。

---

## 3. 命令 / 查询的返回形状

`skill.cast` / `quest.*` / `entity.set_hp` / `query.ask` 的返回值是统一形状的表
（由 `FillScriptReply` 写入，脚本侧无需按 op 分支）：

```lua
local r = skill.cast(caster, skill_id, target)
r.ok   -- boolean：handler 是否成功（ScriptReply.valid）
r.i0   -- integer：主结果（如伤害值）
r.i1   -- integer：次结果（如载荷参数个数）
r.d0   -- number ：浮点结果
```

**为什么脚本拿不到任意结构**：返回值刻意收敛成 4 个标量字段。业务语义由 op handler
决定怎么填，脚本不能凭构造任意表来操纵上层语义。

**失败不返回表**：binding 失败走 Lua 错误机制（见 §5），不会返回一个 `ok=false` 的表。

---

## 4. 脚本模块形态与 `_ENV`

```lua
-- 形态 A：不返回表 → 脚本私有的 _ENV 表就是模块表
function hi() probe.report(11) end

-- 形态 B：返回表 → 该表成为模块表（_ENV 作为其 __index 兜底）
local M = {}
function M.go() probe.report(33) end
return M
```

- `Call(id, "hi")` 在**模块表**里查 `hi`。
- chunk 在**独立 `_ENV`** 中执行（`__index = 沙箱全局表`）：
  脚本 A 的全局写不会污染脚本 B；`Unload` 即整体回收。
- 脚本自带元表的返回表 **不被覆盖**（尊重脚本作者）。

---

## 5. 错误语义

### 5.1 脚本侧

脚本里 `error("...")`、索引 nil、类型错误等，都会被捕获成 `ScriptError`，
**不会让宿主进程崩溃**。当前脚本的这次 `Call` 返回失败，其余脚本与 Scene Tick 照常。

### 5.2 宿主侧错误码往返

绑定失败时，宿主原始错误码会**穿过 Lua 的错误机制回到调用方**：

```
op handler 返回 Fail(NOT_FOUND)
  → CopyErrorText 压成 "lua/NOT_FOUND: entity not found"   （longjmp 前的平凡字节串）
  → lua_error / longjmp
  → lua_pcall 返回 LUA_ERRRUN
  → ParseCoreErrorCode 从错误串还原出 NOT_FOUND            （这是唯一的还原通路）
  → Call 返回 core::Error{NOT_FOUND}
```

所以：

| 脚本里做的事 | `Call` 返回的 `ErrorCode` |
|---|---|
| `entity.get(失效id)` | `NOT_FOUND` |
| `entity.get(0)` / `entity.get("abc")` | `INVALID_ARGUMENT` |
| `entity.set_hp(id, 2.5)` | `INVALID_ARGUMENT` |
| 未绑定 Entity 就 `entity.get(1)` | `INVALID_ARGUMENT` |
| `skill.cast` 参数个数不对 | `INVALID_ARGUMENT` |
| 未注册的 op（`query.ask('nope')`） | `NOT_FOUND` |
| 语法错误 | `INVALID_ARGUMENT`（`CompileError`） |
| 指令/时间超限 | `TIMEOUT` |
| 运行期 `error()` / 类型错误 | `INTERNAL_ERROR`（`RuntimeError`） |
| 栈深超限 | `INTERNAL_ERROR`（`StackOverflow`） |
| 内存超限 | `INTERNAL_ERROR`（`MemoryLimit`） |

`ScriptContext::LastError()` 给出**细分**的 `ScriptError` + 脚本名 + 行号：

```cpp
const auto& e = ctx.LastError();
e.code    // ScriptError::InstructionLimit / Timeout / StackOverflow / CompileError / ...
e.script  // "spin.lua"
e.line    // 12
e.text    // 已剥掉位置前缀的原始文本
```

> 注意 `core::ErrorCode`（9 个，跨进程稳定）与 `ScriptError`（8 个，脚本内部细分）
> 是**两层**：前者给调用方做分支，后者给排障看细节。别拿前者当后者用。

---

## 6. 脚本返回值怎么回传 C++

`Call` 的签名冻结为 `core::Result<void>` —— **不直接回传脚本返回值**。
需要把脚本算出来的数带回来的，走两条受控通路：

1. **事件**：脚本 `event.publish('x', v)` → C++ 订阅者拿到 `ScriptEvent{name, args}`。
2. **受控写**：脚本请求 op（如 `entity.set_hp`），由 **op handler** 真正改状态，
   handler 自己的副作用就是回传结果。

测试里的 `probe.report(v)` 就是第 2 条通路的示范：宿主注册一条 Native 绑定，
在自己的 `user` 指针上收集脚本回传的值。

---

## 7. §13 Tick Safe Point

脚本执行期间**不能**热切脚本。`Load` / `Unload` 在 `run_depth_ > 0` 时不会立即生效，
而是**暂存**到 `pending_`：

```
Compile → Validate → Load → [Safe Point] Activate → Rollback
```

- `PendingCount()` 看暂存了几条。
- `ApplyPendingChanges()` 在安全点统一生效；正在执行脚本时返回 `BUSY`。
- **这次执行因限额被中止 ⇒ 暂存变更整批回滚丢弃**（避免「半次热更」）。
- `Version()` 只在**实际**切换后 +1，供下游判断是否要重建缓存。

---

## 8. 线程模型

- **一个 Scene 一个 VM**，VM 有唯一 owner 线程（创建它的线程）。
- 所有入口（`Load` / `Unload` / `Call` / `ApplyPendingChanges`）都是 **owner-thread-only**；
  从别的线程调用返回 `BUSY`，不崩溃、不静默。
- 不同 VM 之间**不共享任何 Lua 状态**，可在线程上并行跑。
- 绑定函数在**调用它的那个线程**（即 owner 线程）执行，不允许自己再跨线程回调 Lua。

---

## 9. 热路径纪律

| 路径 | 入口 | 约束 |
|---|---|---|
| Hot Path | `Call` | 已编译 chunk 直接跑；无 MySQL / 同步 Redis / 同步 gRPC / 文件 IO |
| Cold Path | `Create` / `Load` / `Unload` / `ApplyPendingChanges` | 允许分配、编译 |

- 指令/时间预算由 debug hook 计量（默认每 1000 条指令一次回调）。
  hook 只影响**中止粒度**，不影响是否会中止。
- `MemoryUsed()` / `MemStats()` / `LimitHits()` 都是 `noexcept` + 原子读，可在热路径采集。

---

## 10. 宿主扩展点（§27.4 注册表机制）

新增脚本能力**不需要改本模块任何文件**：

```cpp
BindingDef def;
def.name        = "myapi.thing";   // 必须是 `ns.fn` 形式
def.kind        = BindingKind::Native;   // 分类只影响统计与自检
def.fn          = &MyThing;              // core::Result<int>(*)(ScriptCall&, void*)
def.user        = &my_state;
def.read_only   = false;                 // Query 类必须 true
def.description = "…";
ctx.AddBinding(std::move(def));
```

同理，业务能力走注册表声明：

```cpp
ctx.RegisterCommandOp("skill.cast", &MySkillHandler, &my_state);
ctx.RegisterQueryOp("player.status", &MyStatusHandler, &my_state);
```

- 重名 / 空函数 → `INVALID_ARGUMENT`（**禁止静默覆盖**）。
- 名册冻结后 `Add` → `BUSY`（防 Tick 期中途行为漂移）。
- 绑定函数签名固定：`core::Result<int> Fn(ScriptCall& call, void* user)`，
  出口统一 `return call.Done();`。

---

## 11. 诊断面（`LuaVM`）

`ctx.Vm()` 暴露观测用读数，全部 `noexcept`：

| 读数 | 含义 |
|---|---|
| `MemoryUsed()` / `PeakMemory()` / `MemoryLimit()` | 计数 allocator 实时 / 峰值 / 上限 |
| `AllocCalls()` / `AllocFailCalls()` | 分配次数 / 触顶次数 |
| `LimitHits()` | 四类限制累计触发次数 |
| `InstructionChecks()` | hook 回调累计次数（指令开销分子） |
| `SandboxOpen()` | 沙箱是否装配成功 |
| `HookPeriod()` / `SetHookPeriod()` | 指令检查周期（**不得用于放宽安全性**） |
| `OwnerThread()` / `OnOwnerThread()` | 线程归属 |
| `LastError()` | `{code, text, script, line}` |

---

## 12. 实测基线（Release / MinGW g++ 16.1.0 / `--calls 1000000`）

| 指标 | 实测 |
|---|---|
| `lua_call_ns` | 112.9 |
| `lua_call_ns_p99` | 200.0 |
| `lua_binding_call_ns`（脚本→C++ 绑定） | 173.4 |
| `lua_load_ms_per_script` | 0.0105 |
| `lua_load_100_ms`（100 个脚本） | 1.054 |
| `mem_overhead_bytes`（空 VM） | 13906 |
| `mem_after_100_scripts` | 99344 |
| `instruction_check_ns` | 3238.8 |
| `instr_overhead_pct` | 1.060 |

验收门槛：`lua_call_ns ≤ 2000`（实测有 18× 余量）。
**数字来自 `bin/lua_bench` 真实执行**，不是估算；重跑会覆盖 `bench/lua.txt`。
