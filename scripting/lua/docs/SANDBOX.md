# Lua 沙箱（Sandbox）

> TASK-031 · Lua Runtime —— 沙箱设计与装配契约。
> 对应规范：**§8 线程/Tick 模型 · §15.7 脚本安全 · §21 沙箱禁止项 · §25 确定性测试**。
> 本文档与 `INTERFACE.md` 互为补充：`INTERFACE.md` 讲**脚本能调用什么**（绑定契约），
> 本文档讲**脚本能碰到什么**（能力边界）。

---

## 1. 设计立场：白名单，而不是黑名单

朴素做法是「先把标准库全开，再把 `io` / `os` / `debug` / `require` 逐个置 `nil`」。
这是**黑名单**，有两个致命问题：

1. **静默失效**：Lua 一旦新增某个能力，或某个库改为惰性初始化，黑名单不会自动覆盖
   —— 沙箱被悄悄绕过，而**没有任何报错**。
2. **难以审计**：要证明「安全」，必须穷举所有被禁项；只要漏一个就前功尽弃。

本实现改成**白名单三步重建**：

| 步骤 | 动作 | 效果 |
|---|---|---|
| 1 | 只打开 **base / string / table / math** 四个库 | `io` / `os` / `package`(require) / `debug` / `coroutine` **从未打开**，不是「被置 nil」而是**根本不存在** |
| 2 | 用**显式成员白名单**重建 `string` / `table` / `math` 三张库表 | 白名单外的成员（如 `string.dump`）在新表里**不存在** |
| 3 | 用**显式名字白名单**重建整张全局表并写回 `LUA_RIDX_GLOBALS` | 原全局表不可达，其上的 `load` / `loadfile` / `dofile` / `collectgarbage` / `warn` 一并消失 |

**结论**：「脚本能碰到什么」完全由下面 §2 的常量表决定。新增 Lua 能力**不会**自动进沙箱
—— 必须显式加入白名单才可见。这是 §21「白名单而非黑名单」的落地点。

---

## 2. 白名单常量表（唯一事实源）

常量定义在 `scripting/lua/src/sandbox.cpp`（匿名命名空间，编译期常量，不可运行期修改）。

### 2.1 全局表白名单 `kGlobalWhitelist`（23 项）

```
assert  error  getmetatable  ipairs  math  next  pairs  pcall  print
rawequal  rawget  rawlen  rawset  select  setmetatable  string  table
tonumber  tostring  type  xpcall  _G  _VERSION
```

**刻意排除**（含原因）：

| 排除项 | 原因 |
|---|---|
| `load` / `loadfile` / `dofile` | 等价于 `loadstring` + 文件 IO；与 `string.dump` 组合可绕过源码审计 |
| `collectgarbage` | 允许脚本强行驱动 GC，可挪用 CPU 预算、破坏 Tick 时间上界 |
| `warn` | 直写 stderr 的 IO 通道 |
| `io` / `os` / `package` / `debug` / `coroutine` | **从未开库**，天然不存在 |

### 2.2 `string` 白名单 `kStringWhitelist`（16 项）

```
byte  char  find  format  gmatch  gsub  len  lower
match  pack  packsize  rep  reverse  sub  unpack  upper
```

**排除 `string.dump`**：字节码序列化。拿到它即可把编译产物落盘、或与 `load` 组合
绕开源码级审计（`dump` 直接暴露 Lua 内部字节码，削弱沙箱假设）。

### 2.3 `table` 白名单 `kTableWhitelist`（7 项）

```
concat  insert  move  pack  remove  sort  unpack
```

全部成员均无 IO、无越权能力，故整体放行。

### 2.4 `math` 白名单 `kMathWhitelist`（26 项）

```
abs  acos  asin  atan  ceil  cos  deg  exp  floor  fmod  huge  log
max  maxinteger  min  mininteger  modf  pi  rad  random  sin
sqrt  tan  tointeger  type  ult
```

**排除 `math.randomseed`**：见 §5。

---

## 3. 装配顺序（顺序有语义，不可交换）

`LuaVM::OpenSandbox()` → `lua_pcall(LuaSandboxSetup)`，装配体内部严格按此顺序：

```
1. OpenWhitelistedLibs()                  // 只开 base/string/table/math
2. SeedDeterministicRandom(0x2545F4914F6CDD1D)
                                          // 必须在重建 math 之前（randomseed 即将消失）
3. string: RebuildLibrary → 修字符串元表 __index → lua_setglobal
4. table : RebuildLibrary → lua_setglobal
5. math  : RebuildLibrary → lua_setglobal
6. RebuildGlobals()                       // 必须最后：前几步都还要用「原」全局表
```

**为什么 `RebuildGlobals` 必须最后**：第 3–5 步的输入是**当前全局表**里的
`string` / `table` / `math`（由第 1 步开库产生）。若先重建全局表，这三张表就被搬到新表，
而白名单版库表又需要写回 `_G`，顺序纠缠会互相覆盖。

**为什么 `SeedDeterministicRandom` 必须在重建 `math` 之前**：重建后 `math.randomseed`
已不在白名单内，脚本无法再取到它 —— 种子由宿主一次性固定，之后不可变（§5）。

---

## 4. 字符串元表逃逸点（实测踩坑，已修，勿回退）

**这是白名单方案最容易漏的一个洞。**

Lua 的字符串类型自带元表，其 `__index` 指向的是**原本那张 `string` 表**
（由 `luaopen_string` 创建）。因此即使把白名单版 `string` 表 `lua_setglobal("string", ...)`，
脚本仍可用**方法调用语法**拿到被排除的成员：

```lua
local d = ("x").dump        -- 走字符串元表 __index，绕过全局白名单！
```

**修复**：在 `lua_setglobal("string", ...)` **之前**，显式把字符串元表的 `__index`
重定向到白名单版 `string` 表：

```cpp
(void)lua_pushliteral(state, "");
if (lua_getmetatable(state, -1) != 0) {
    (void)lua_pushvalue(state, fresh_string);
    lua_setfield(state, -2, "__index");  // mt.__index = 白名单版 string
    lua_pop(state, 1);
}
lua_pop(state, 1);
lua_setglobal(state, LUA_STRLIBNAME);
```

修复后 `("x").dump` 与 `string.dump` 行为一致 —— 都是 `nil`。

---

## 5. 确定性随机（§25 Replay / Determinism）

`math.random` 由宿主 VM 创建时用**固定种子** `0x2545F4914F6CDD1D` 播种一次。
`math.randomseed` 不在白名单内 → **脚本永远无法重新播种**。

理由：Replay Test 与 Determinism Test（§25）要求「同一输入序列 → 同一输出序列」。
若脚本可在 Tick 中途重新播种，随机序列将依赖执行时机而非输入，回放必然发散。

> 语义边界：本机制只保证**同一种子 → 同一序列**。跨平台浮点差异、以及脚本自身
> 依赖字典序遍历（`pairs`）的部分，仍须遵守 `INTERFACE.md` 的确定性约定。

---

## 6. `allow_io` / `allow_loadstring` 的真实语义

`LuaLimits` 中有两个布尔字段（见 `lua_vm.h`）：

| 字段 | 默认 | 真实语义 |
|---|---|---|
| `allow_io` | `false` | **恒为 false 生效**。`io` / `os` 库从未开库；本字段是**显式声明位**，置 `true` **不改变**白名单 |
| `allow_loadstring` | `false` | 同上。`load` / `loadfile` / `dofile` 不在全局白名单内，置 `true` 也不出现 |

**为什么保留这两个字段而不是删掉**：它们是**契约声明**——把「本运行时不允许 IO / 不允许
loadstring」写进类型系统，让调用方与审计者一眼可见；同时为未来若需放开（走 Architecture
Change Request，§35）预留稳定字段位，避免破坏 `LuaLimits` 的字段冻结约定（§27.1）。

**注意**：置 `true` **不是**「放开开关」，而是「无效声明」。真正的放开必须改
`sandbox.cpp` 的白名单常量 —— 那是架构级变更，须走 RFC。

---

## 7. `_G` 自引用

`RebuildGlobals` 会把新全局表的 `_G` 字段指向**新表自身**：

```cpp
(void)lua_pushvalue(state, fresh);
detail::RawSetField(state, fresh, "_G");
```

于是脚本里 `_G.x = 1` 写的是**沙箱全局表**，而不是那个已不可达的原表。
若漏掉这一步，`_G` 会解析为 `nil`（白名单表里没有 `_G` 键），脚本一用就报
`attempt to index a nil value (global '_G')`。

最后一步 `lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS)` 才是真正换环境的关键：
Lua 5.4+ 的 `lua_getglobal` / `lua_setglobal` / `lua_pushglobaltable` 都从该 registry 槽取值
（`lapi.c`），写回槽位即完成全局表替换。

---

## 8. 装配失败语义（§19 不崩溃）

`LuaSandboxSetup` 是 C 函数，整体包在 `lua_pcall` 内执行。原因：装配期同样需要分配内存，
若计数 allocator 触顶，Lua 会抛 `LUA_ERRMEM`；包在 pcall 里就变成**可恢复错误**，
而不是走到 panic → `abort()`。

三种失败路径均返回 `INTERNAL_ERROR`，**绝不抛异常、绝不崩溃**：

| 情况 | 返回 |
|---|---|
| `lua_pcall` 非 `LUA_OK`（如装配期 OOM） | `INTERNAL_ERROR` `"lua sandbox setup failed"` |
| 某张库表重建失败（`RebuildLibrary < 0`） | `INTERNAL_ERROR` `"lua sandbox rebuilt lib missing"` |
| 全部成功 | `Ok()`，`sandbox_open_ = true` |

失败时 `sandbox_open_ = false`；`SandboxOpen()` 可供上层判定。

---

## 9. 装配踩坑清单（实测，勿回退）

### 9.1 全局白名单漏掉 `string` / `table` / `math` → 运行期才炸

装配顺序是「先 `lua_setglobal` 装入白名单版三张库表，最后 `RebuildGlobals` 按
`kGlobalWhitelist` 从**原全局表**逐个搬」。若 `kGlobalWhitelist` 漏掉这三张表，
搬过去的新全局表里就没有它们 —— 脚本一写 `string.find(...)` 就报：

```
attempt to index a nil value (global 'string')
```

**危险性**：`OpenSandbox()` 本身**返回 OK**（沙箱装配成功），错误只在脚本真正运行时才暴露。
属于「静默装配成功 + 运行期必炸」的隐蔽 bug。

### 9.2 `RebuildLibrary` 返回过期索引 → 无限 `__index` 链

```cpp
const int orig = lua_gettop(state);
const int fresh = BuildWhitelistedFrom(state, orig, names, count);  // [orig][fresh]
lua_remove(state, orig);                                            // [fresh]
return lua_gettop(state);   // 修复：remove 后 fresh 已过期，取真实栈顶
```

`lua_remove(orig)` 会把 `orig` 之上的元素整体下移一位，因此 `BuildWhitelistedFrom`
返回的 `fresh` **已过期（比真实索引大 1）**。直接 `return fresh` 会让调用方
`lua_pushvalue(state, fresh)` 压到隔壁那个值：

- 现象：字符串元表 `__index` 被设成**空字符串**而不是白名单版 string 表；
- 后果：`("x").dump` 索引字符串时又回到同一张元表 → **无限 `__index` 链** →
  运行期报 `'__index' chain too long; possible loop`。

### 9.3 `lua_rawset` 键值顺序（通用陷阱）

`lua_rawset` 语义要求**键在 top-2、值在 top-1**。本模块所有写字段一律走
`detail::RawSetField`，其内部用 `lua_insert(-2)` 换位；若手写 `lua_rawset` 而
先压值后压键，会写成 `t[value] = key`（键值颠倒），表现为「读回来全是 nil」。
见 `internal.h` 的 `RawSetField`。

### 9.4 `extern "C"` 包裹 Lua 头（Lua 5.5 专属，跨平台必踩）

**Lua 5.5 的 `lua.h` 不含 `extern "C"` 守卫**（Lua 自己的 `lua.hpp` 才提供）。
从 C++ 直接 `#include <lua.h>` 会导致 `lua_*` 符号被 C++ 名字修饰：

```
undefined reference to `_Z10lua_gettopP9lua_State'
```

修复：`internal.h` 中统一包裹：

```cpp
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}
```

---

## 10. 验证证据

| 验证项 | 方式 | 结果 |
|---|---|---|
| 沙箱生效：`io` / `os` / `require` / `loadstring` 不可用 | 单元测试断言 | 全绿 |
| `string.dump` 与 `("x").dump` 同时为 `nil` | 单元测试断言（元表逃逸点回归） | 全绿 |
| 新全局表含 `string` / `table` / `math` / `_G` | 单元测试断言 | 全绿 |
| 装配失败不崩溃 | 装配体包 `lua_pcall`，失败返回 `INTERNAL_ERROR` | 代码路径 |
| 确定性随机：脚本无法 `randomseed` | `kMathWhitelist` 不含 `randomseed` | 代码常量 |

配套交付物：`scripting/lua/include/mmo/script/lua_vm.h`（`LuaLimits` / 装配入口）、
`scripting/lua/docs/INTERFACE.md`（绑定契约与错误语义）。

---

## 11. 修改沙箱的正确姿势

白名单常量是**安全边界**，修改属于架构级变更：

1. 新增全局名 / 库成员 → 改 §2 对应常量表，并在本文件更新表格与理由；
2. 放开 `allow_io` / `allow_loadstring` 的**实际效果** → 必须走 Architecture Change
   Request（§35），因为这会推翻 §21 的沙箱假设；
3. 任何改动后，**必须**同步给单元测试补一条「该名字可见 / 不可见」的断言 ——
   沙箱改动没有测试等于没有改动。
