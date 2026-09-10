# TASK-032 · Lua Hot Reload —— 架构与运维手册

> 模块：`scripting/lua`　｜　交付物：`include/mmo/script/hot_reload/` + `src/hot_reload/` +
> `tests/` + `benchmark/` + `tools/scriptctl/` + `docs/script-versions.md`
> 依赖：TASK-003（Core Time / UUID / Config）· TASK-013（Simulation Scheduler）· TASK-031（Lua Runtime）
> 前置阅读：`scripting/lua/docs/INTERFACE.md`（脚本运行时契约）· `scripting/lua/docs/SANDBOX.md`（沙箱白名单）

---

## 1. 目标与不可协商的约束

热更新的目标形态是 **Load → Compile → Validate → Activate → Rollback → Version** 六阶段，
但真正的难点不在"换掉一段代码"，而在 §21 明文禁止的三件事：

| 禁止项 | 后果 | 本设计的对策 |
|---|---|---|
| 在 Tick 执行中间替换脚本 | 同一 Tick 内不同系统看到不同版本 → 状态机撕裂 | **Tick Safe Point** 门（§4），非安全点一律 `BUSY` |
| 热更时重建 VM | 脚本全局状态（计数器 / 缓存 / 已建连接表）全部丢失 | `ScriptContext::ReloadInPlace` **复用旧 `_ENV`**（§5），只换模块表 |
| 热更失败让线上半新半旧 | 无法用"旧版本还在服务"兜底 | 阶段 1-3 全程在**隔离临时 VM**，生产 VM 零接触（§2） |

外加两条性能红线：单次 `Activate` 停顿 **< 100us**（§22），版本历史**有界**（保留最近 5 个，禁止无限增长）。

---

## 2. 六阶段流水线

```
        任意线程（Worker / 运维）                     SimulationThread（安全点）
  ┌──────────────────────────────────────┐   ┌────────────────────────────────────┐
  │ 1 Load      读取源码（内联 / 文件）    │   │                                    │
  │ 2 Compile   lua_load 编译             │   │                                    │
  │ 3 Validate  静态扫描 + 隔离 VM 冒烟    │   │                                    │
  │             ↓ 失败 = 生产 VM 从未被触碰 │   │                                    │
  │             产出 ReloadTicket（纯值）──┼──▶│ 4 Activate ★原子替换★               │
  └──────────────────────────────────────┘   │      ReloadInPlace / Load          │
                                             │ 5 Verify   下一安全点冒烟，失败自动回滚│
                                             │ 6 Commit   记录 ScriptVersion（内存队列）│
                                             └────────────────┬───────────────────┘
                                                              │ Tick 之外
                                                  7 DrainAudit → IAuditSink（文件 / DataService）
```

状态机（`ReloadState`，**每脚本一份**）：

```
Idle ──Prepare──▶ Compiling ──▶ Validating ──┬─ ok ──▶ PendingActivate ──Activate──▶ Activated
                                             │                              │
                                             └─ 失败 ─▶ Failed ◀────────────┤
                                                                             ▼
                                                          Rollback / 自动回滚 ─▶ RolledBack
```

### 为什么 `ReloadTicket` 只携带「纯值」

Lua 的编译产物（`lua_load` 出的 chunk 与其 registry 引用）**绑定到具体的 `lua_State`**。
生产 VM 属 SimulationThread，Worker 线程在临时 VM 上编译出来的 chunk 对它无效。
所以票证里只放**源码 + checksum + 校验报告**，`Activate` 时在生产 VM 上重新编译一次。
这笔开销是实测过的：单脚本编译 ≈ 8us（TASK-031 `lua_load_ms_per_script=0.0080`），
远低于 100us 预算；换来的是"阶段 1-3 完全不碰生产 VM"这条硬保证。

### 阶段 3 的静态扫描必须先剥离注释与字符串

`ScanBanned` 在 `StripLuaTrivia` 之后按**标识符边界**匹配。原因：裸子串扫描会把注释里的
`-- load the config from disk`、字符串里的 `'io os require'` 误判为违规。**误报比漏报更有害**
——它会让完全合法的脚本被拒绝上线，而运维会因此把校验关掉（`validate_before_activate=false`），
结果连真违规也放行了。实测：加了剥离之后，注释/字符串里的禁用词不再触发，而真实调用
`io.open('x')` 仍被拦下（单测 `TestFailuresKeepOldVersionServing` 两端都断言）。

禁用名单（`kBannedApis`）是**沙箱白名单的补集**，并不代表"能调用成功"：沙箱里它们本来就是
`nil`，命中只说明脚本作者写错了 API —— 提前拦截避免上线后整段逻辑静默失效。

---

## 3. 线程模型（§9，硬约束）

| 方法 | 允许的线程 | 说明 |
|---|---|---|
| `Prepare` / `PrepareFromFile` / `Validate` | **任意线程** | 只碰隔离临时 VM |
| `Activate` / `Rollback` / `VerifyPass` / `ActivatePending` | **仅 SimulationThread 且处于安全点** | 直接改生产 VM |
| `CurrentVersion` / `History` / `StateOf` / 各计数器 | 任意线程 | `mu_` 保护，只读 |
| `DrainAudit` | 任意线程，**但禁止在 Tick 内** | 会做文件 IO（§11） |

`Activate` 的门是**三重**的，缺一即 `BUSY`：

```cpp
if (!InSafePoint())                      → BUSY   // §21 禁止 Tick 中途替换
if (!context_.Vm().OnOwnerThread())      → BUSY   // §9  禁止跨线程碰 lua_State
if (context_.InScriptExecution())        → BUSY   // §19 正在执行该脚本 → 等安全点，不打断当前调用
```

第四道门是 **`UNAUTHORIZED`**：只有"本对象内部记录了该 `(name, checksum)` 已通过校验"才放行。

> **踩坑（必读）· 判据必须是内部记录，不能是入参 `ticket.report`**
> `Validate` 按 §7 冻结签名**按值**接收 `ReloadTicket`，它填好的 `report` 只留在
> `entries_[name].pending` 里，**无法回传**给调用方手上那份副本。因此若在 `Activate` 里以
> `ticket.report.ok` 为判据，标准流程 `Prepare → Validate → Activate`（原样传 `Prepare`
> 返回的那张票证）会**必然**报 `UNAUTHORIZED` —— 实测确实踩到过。
> 更糟的是，若要求调用方手工回填 `report`，等于把"是否已校验"交给调用方自述，那正是伪造开关。
> 现设计以 `(name, checksum)` 对齐的内部记录为唯一判据：标准流程自然通过，
> 且**手工构造一张 `report.ok = true` 的假票证不会通过**（单测 `TestActivateGateAndThreadModel`
> 第 1 条即此断言）。

---

## 4. Tick Safe Point

### 4.1 门本身

```cpp
reloader.BeginSafePoint(tick_number);   // SimulationThread
// … 此窗口内允许 Activate / Rollback / VerifyPass / ActivatePending …
reloader.EndSafePoint();
```

`SafePointTick()` 记录当前 Tick 号，供阶段 5 判定"切换到下一个 Tick 才算完成"
（`entry.activated_tick < current_tick`）。若允许同 Tick 内刚切完就立刻冒烟，会把
"切换瞬间"误当成稳定态，也让"切换前后不在同一 Tick"失去判据。

### 4.2 一行接入 TASK-013

```cpp
auto stage = std::make_unique<mmo::script::ScriptReloadStage>(reloader);  // 默认 TickPhase::Replication
sched.RegisterStage(std::move(stage));
```

`ScriptReloadStage::Execute` 做四件事，全部无 IO：
`BeginSafePoint(ctx.tick_number)` → `ActivatePending(trace)` → `VerifyPass(trace)` → `EndSafePoint()`。

**审计落盘刻意不在这里做** —— 那是文件 IO，混进 Tick 内既违反 §11，也必然顶破 100us 停顿预算。
宿主在 Tick 之外调 `DrainAudit()`。

---

## 5. 原子替换：为什么不重建 VM

`ScriptContext::ReloadInPlace(name, source)`（TASK-032 加法扩展，不改既有 `Load` 语义）：

- **必须已装载**，否则 `NOT_FOUND`（热替换不是装载，缺失即报错，不静默新建）；
- **复用旧脚本的私有 `_ENV`**：把旧 `_ENV` 里的**非函数值**浅拷贝进新 `_ENV`，然后在新环境执行新 chunk；
- **丢弃旧函数**：否则 `_ENV` 的 `__index` 链每热更一次就长一层，且旧函数残留会造成"半旧"；
- 失败（语法错 / 顶层执行错）时**旧模块表、旧 `_ENV`、版本号原样保留**（§20 验收 #2）。

`HotReloader::SwapIn` 按状态自动选路：已装载 → `ReloadInPlace`；否则 → `Load`。
`entry.current_id` 由 `HotReloader` 自己捕获，因为 `ScriptContext` 只提供 `NameOf(id)`、
**没有** `IdOf(name)`，而阶段 5 冒烟要按 id 调 `Call`。

> **踩坑（必读）· 状态保留会"正确地"击败 `or` 默认值**
> 脚本常量千万不要写 `multiplier = multiplier or 100`。热更时新环境里已经有从旧环境拷来的
> `multiplier = 10`（真值），`or` 于是永远不取右边 —— 新逻辑根本不生效。
> 实测现象：集成测试 `v1_calls=12 v2_calls=0`，看起来像"热更没生效"，实际是**测试脚本写法
> 与状态保留语义冲突**。常量的更新必须**无条件赋值**；只有累积型状态才用 `x = x or <初值>`。

> **踩坑 · 环境变量/全局表要不要一起保留？** 当前实现只浅拷贝**非函数值**。嵌套 table
> 是共享引用（不是深拷贝），所以脚本里对表的原地修改会被新版本继续观察到 —— 这通常正是
> 期望行为（缓存/计数器），但若脚本把整个表**重建**赋值，则表现为"状态重置"。见 §8 已知边界。

---

## 6. 自动回滚与版本历史

- **阶段 5（VerifyPass）**：在安全点内对"上一 Tick 激活"的脚本调一次 `config_.smoke_function`
  （默认 `__hot_smoke`）。失败 → 自动回滚到上一个 checksum **不同**的版本，`AutoRollbackCount()`
  递增，Scene 继续跑（§19 / §20 验收 #4）。
  脚本未定义冒烟函数 → 视为**通过**（并非所有脚本都需要自检入口；`NOT_FOUND` 不算失败）。
- **回滚选版本**：不直接取 `history[1]`，而是找"最近一个 checksum 与当前不同"的版本 ——
  否则刚回滚过再回滚会选到与当前完全相同的源码，白增一个版本号。
- **历史有界**：`history` 元素**含源码**，故硬截断到 `max_rollback_versions`（默认 5）。
  实测内存：热更 1 次后 `18091` 字节 → 连续 12 次后 `31288` 字节（单测输出），有界不发散。
- **已回滚版本不再验证**：`needs_verify = !is_rollback`，否则会陷入"验证失败 → 回滚 → 再验证"的死循环。

---

## 7. 审计与持久化口径（§13）

阶段 6 拆成两半，因为"记录"必须够快、"落盘"必须够慢：

| 半 | 位置 | 代价 |
|---|---|---|
| 记录进内存队列 | `ActivateLocked` 内（**安全点内**） | 纳秒级，无 IO |
| 交给 `IAuditSink` 落盘 | `DrainAudit()`（**Tick 之外**） | 文件 / 网络 IO |

`MarkdownAuditSink` 写 `docs/script-versions.md`，按 `(脚本名, 版本, checksum)` **去重**，
构造时读既有文件恢复去重键集合。这一点很关键：审计日志是**被提交进仓库的制品**，
若每次运行都追加一行带时间戳的新记录，工作区会永远 dirty、每次 diff 都是噪声，
审计日志本身也就失去了"差异即变更"的信号。
实测幂等性：首次运行 `audit_records=2 written=2`，再跑一次 `written=0` 且文件 md5 不变。

落盘失败（`Append` 返回错误）时记录**留在队列里等重试**，绝不静默丢审计（§21 禁止无版本记录的热更）。

> **口径说明**：`checksum` 是自包含的 **FNV-1a 64**（16 位十六进制），用途是"同一版本去重 /
> 变更检测"，**不承担防篡改职责**。本模块依赖集不含 security / economy（§27.2），不得跨模块
> 包含他方 `src/` 的 SHA-256 实现。要防篡改需在 DataService 侧对脚本内容签名校验。
>
> **运行期持久化留白**：§13 要求"持久化只能经 DataService"。TASK-032 的依赖集（003/013/031）
> **不含** DataService（TASK-026），故本任务只提供 `IAuditSink` 抽象 + 文件实现；落库实现
> 由后续任务提供（实现 `IAuditSink` 并经 DataService 写入即可，`HotReloader` 侧零改动）。

---

## 8. ⚠ 架构缺口报告：TASK-013 没有 Tick 边界一等钩子

> **这是需要人工决策的事项，不属于本任务可自行决定的范围。本任务未修改 TASK-013。**

### 事实（硬证据）

任务书 §15 第 4 步要求"向 TASK-013 SimulationScheduler 注册 Tick 边界钩子"。实测其公开接口
**不支持**：

1. `SimulationScheduler::FireTick`（`server/gamenode/scheduler/src/simulation_scheduler.cpp:53`）
   固定跑完 `kTickPhaseOrder` 的 8 个阶段，然后 `events_.Drain(...)` 收尾 —— **中间没有可注册的回调点**，
   也没有"八阶段全部完成之后"的钩子。
2. `RegisterStage` 对**同 Phase 重复注册直接报错**（§21 禁止静默覆盖），而
   `TickPhase`（`tick_phase.h`）只有 8 个值 + `Count`，**不存在第 9 个 "SafePoint" 相位**。
3. §27.3 明令本任务只能改自身 `module` 子树，且禁止在 TASK-013 `STATUS: DONE` 后静默改其接口签名。

### 本任务的降级方案（已落地）

把安全点实现为**最后一个阶段**（默认 `TickPhase::Replication`）：轮到它执行时，前面 7 个阶段
都已跑完，紧接着就是事件 drain 与下一 Tick 的 Input —— **它就是这个 Tick 的边界**。

语义等价性由集成测试实测证实：

```
[info] ticks=12 activation_tick=6 first_v2_tick=7 switch_at_call=6 v1_calls=6 v2_calls=6
[info] max_tick_us=0 overrun=0
```

即：激活发生在 Tick 6 的 Replication 阶段；Tick 6 的 Combat 阶段仍看到 v1（第 6 次调用 = 60）；
第一次看到 v2 是 Tick 7（`first_v2_tick = activation_tick + 1`）—— **切换前后版本绝不混在同一个 Tick 里**。
`max_tick_us=0` 说明安全点内的替换没有造成任何 Tick 尖峰。

### 唯一残留风险（为何仍需 ACR）

`Replication` 相位是**可能被其他系统占用**的公开相位。当前 `FireTick` 只迭代**已注册**的阶段，
故本适配器独占该相位是可行的；但一旦有别处也注册 `Replication`，`RegisterStage` 会报
`duplicate phase`，热更会**静默失去安全点**（表现为永不激活）。宿主的两条兜底：

- 换一个空闲相位构造：`ScriptReloadStage(reloader, TickPhase::Event)`；
- 或者干脆不用本适配器，在自家 Tick 循环的边界处直接调
  `BeginSafePoint/ActivatePending/VerifyPass/EndSafePoint`（三行，`ScriptReloadStage` 做的事）。

### 建议的正确做法

给 TASK-013 增加一等 `SafePointHook`（走 **Architecture Change Request**）：在 `FireTick` 的
阶段循环之后、`events_.Drain` 之前插一个可选回调。届时本适配器可直接替换为那个钩子，
`HotReloader` 侧**零改动**。

---

## 9. 实测数字（Release `-O3`，本机 MinGW MSYS2 g++ 16.1.0）

验收命令：`bin/hotreload_bench --scripts 100 --reloads 10`（100 脚本 × 10 轮热更 = 1000 次真实激活）

**以 `scripts/verify/task-032.sh` 现场输出为准**（下表「实测」列即验收脚本内那一次运行的值；
括号内为**三次验收运行**（`task-032.sh` ×2 + `task-done.sh` 内的重跑）的波动区间，
用于说明数字不是单次幸运值）：

| 指标 | 实测（验收现场） | 波动范围 | §22 目标 | 判定 |
|---|---|---|---|---|
| `prepare_ms`（单脚本 Prepare，隔离 VM 装配 + 编译） | **0.0254 ms** | 0.0254–0.0275 | < 10 ms | ✅ 393× 余量 |
| `validate_ms`（单脚本 Validate，静态扫描 + 冒烟） | **0.0256 ms** | 0.0256–0.0279 | < 20 ms | ✅ 781× 余量 |
| `activate_us`（单次 Activate，安全点内原子替换） | **6.096 us** | 6.096–7.792 | **< 100 us** | ✅ **验收门槛** |
| `activate_us_p99` | **41.700 us** | 41.7–49.2 | — | ✅ |
| `rollback_us`（单次 Rollback） | **6.245 us** | 6.245–6.929 | < 100 us | ✅ |
| `activate_batch_ms`（一次安全点内激活全部 100 脚本） | **0.6116 ms** | 0.61–0.78 | — | ⚠ 见下 |
| 内存（1 次热更 / 连续 12 次） | 18091 B / 31288 B | — | 有界 | ✅ |

参数复核：`activations=1200`（100 首装 + 1000 热更 + 100 回滚）· `rollbacks=100` —— 与预期完全一致。

**集成测试实测输出**（`ctest -R HotReload`，§17 的 Tick 边界证据）：

```
[info] ticks=12 activation_tick=6 first_v2_tick=7 switch_at_call=6 v1_calls=6 v2_calls=6
[info] max_tick_us=0 overrun=0
[info] audit_records=2 written=0 path=docs/script-versions.md
```

`activation_tick=6` / `first_v2_tick=7` ⇒ 切换落在 Tick 边界且只影响下一个 Tick（版本不混）；
`overrun=0` ⇒ 安全点内的替换没有造成一次 Tick 超时；
`written=0` ⇒ 审计日志在**整轮验收（含 ctest + bench）之后 md5 未变**，幂等成立。

> **⚠ 关于 `activate_batch_ms`：批量激活会顶破 §21 的"禁止 Tick 尖峰"**
> 100 个脚本同处一个安全点 = 0.61ms 停顿，单 Tick（50ms @20Hz）占比 1.2% —— 不致命，
> 但它**随脚本数线性增长**，而且 8 阶段总预算并不是只有热更在用。
> **生产建议**：再加一层**分片激活**（每次安全点只激活 `N = 8~16` 个），
> 把单 Tick 停顿稳定在 ~0.1ms 量级；本任务只提供 `Activate` 单次（实测 6.1us）与
> `ActivatePending` 批量两个原语，分片策略属宿主调度选择。
> 验收断言只针对**单次** `activate_us ≤ 100`，与任务书 §18/§22 口径一致。

---

## 10. CLI：`tools/scriptctl/`

五个子命令（§15 第 9 步）：`reload` / `validate` / `rollback` / `history` / `status`。

```bash
# 只校验、不上线（最常用：上线前的 CI 门禁）
bin/scriptctl validate --script scripts/damage_formula.lua

# 校验 + 安全点内激活（宿主进程内接入时用 HotReloader；独立进程模式见 --dry-run）
bin/scriptctl reload   --script scripts/damage_formula.lua --name damage_formula --dry-run

# 回滚 / 查历史 / 查状态
bin/scriptctl rollback --name damage_formula
bin/scriptctl history  --name damage_formula
bin/scriptctl status
```

`scriptctl` 是**同进程工具**：它构造一个 `ScriptContext` + `HotReloader`，因此 `reload` 的
"激活"只对这个工具自己的 VM 生效，不能改已在线进程的脚本 —— **这是刻意的**（§4 同一实时状态
只能有一个权威写入者；跨进程改脚本必须走控制面 Command）。
真正要热更在线进程，请用 `ScriptReloadStage` 在进程内接入 + 控制面触发 `Prepare/Validate`。
`scriptctl` 的价值在于：**在 CI / 上线前把不合法的脚本挡在门外**，并提供可读的历史与状态视图。
详见 `tools/scriptctl/README.md`。

---

## 11. 踩坑清单（本任务新增，已回写 SKILL.md §3.8）

| # | 坑 | 正解 |
|---|---|---|
| 1 | `Activate` 以入参 `ticket.report.ok` 为判据 → 标准流程必然 `UNAUTHORIZED` | 判据用**内部** `(name, checksum)` 已校验记录（`Validate` 按值收票证，回写不了） |
| 2 | 脚本常量写 `x = x or <新值>` → 热更时被旧环境里的真值"正确"击败，新逻辑不生效 | 常量**无条件赋值**；只有累积状态用 `x = x or <初值>` |
| 3 | 逐 Tick 观测切换点，但有一个 Tick 在监视循环外驱动 → 激活漏拍、Tick 归因后移一格 | **所有** Tick 一律经同一个"驱动 + 观测"入口 |
| 4 | 测试里写死"第 2 次调用应得 10" → 被拒的热更不会重置脚本 `counter`，实际是 20 | 期望值随调用次数递增（这本身就是"状态没被打断"的证据） |
| 5 | 审计日志每次运行都追加带时间戳的行 → 仓库永远 dirty | Sink 按 `(名, 版本, checksum)` **去重**，构造时读文件恢复键集合 |
| 6 | 反复跑验收把上一轮**失败**运行的审计行留在制品里 | 出干净的制品前先把日志重置到表头，再跑一次通过的用例 |
| 7 | 嵌套 `struct Config` + NSDMI + 外层默认实参 `= {}` → GCC 报 `could not convert '<brace-enclosed initializer list>()'` | 显式默认构造 + 初始化列表（照抄 `SimulationScheduler::Config`） |
| 8 | `Result::Value()` 是 **const 引用**，改它等于改临时量（静默无效） | 先 `T v = r.Value();` 再改；移动 `unique_ptr` 用 `std::move(r).Value()` |
| 9 | `Time` 单位混用（`DurationMs` 直接吃纳秒） | `DurationMs{ns / core::kSteadyNsPerMilli}` |
| 10 | 静态扫描不剥离注释/字符串 → 合法脚本被误拒（比漏报更有害） | `StripLuaTrivia` 后再按标识符边界匹配 |
| 11 | 隔离 VM 不装宿主绑定 → 用绑定面的脚本冒烟必然"索引 nil"失败 | `SetIsolatedPreparer` 装与生产 VM **相同**的绑定面 |
| 12 | 环境 `__index` 链每热更长一层 + 旧函数残留 | 复用 `_ENV` 但**只拷非函数值**，旧函数丢弃 |

---

## 12. 已知边界（明确不承诺的能力）

| 边界 | 说明 | 后续 |
|---|---|---|
| 无跨进程热更 | `HotReloader` 只作用于本进程的 `ScriptContext`（§4 单一权威写入者） | 控制面 Command（TASK-040 方向） |
| 运行期落库未实现 | 只提供 `IAuditSink` 抽象 + 文件实现 | 依赖 DataService（TASK-026） |
| 嵌套 table 是共享引用，非深拷贝 | 脚本**原地改**表 → 新版本可见；脚本**重建**表 → 表现为状态重置 | 若需要真快照，扩展 `ReloadInPlace` 为可选深拷贝 |
| `checksum` 非加密 | 只做变更检测，不防篡改 | DataService 侧签名校验 |
| 分片激活策略未内建 | `ActivatePending` 一次激活全部待生效脚本 | 宿主按 `N=8~16` 分片；见 §9 注 |
| 安全点依赖"最后一个相位" | TASK-013 无一等钩子；相位被占用则热更失效 | 走 ACR 增加 `SafePointHook`，见 §8 |
