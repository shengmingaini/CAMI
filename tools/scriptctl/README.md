# scriptctl —— Lua 热更运维 CLI（TASK-032）

五个子命令：`reload` / `validate` / `rollback` / `history` / `status`。

构建产物：`build/<Debug|Release>/bin/scriptctl.exe`（由 `scripting/lua/CMakeLists.txt` 从
`tools/scriptctl/main.cpp` 接入，方式与 `tools/migrate/main.cpp` → `mysql_migrate` 一致）。

---

## ⚠ 作用域（先读这条，否则会误用）

`scriptctl` 是**同进程工具**：它自建一个 `ScriptContext` + `HotReloader`。因此
`--commit` 的激活**只对这个工具自己的 VM 生效，不能改已在线进程里的脚本**。

这不是偷懒，而是架构要求（PROJECT_REQUIREMENTS §4 / §12）：同一实时状态只能有一个权威
写入者；跨进程直接改对方内存是禁止的。**在线进程内热更**应由宿主用
`ScriptReloadStage` 接入（见 `scripting/lua/docs/HOTRELOAD.md` §4），由控制面 Command 触发。

那么 `scriptctl` 的价值在哪？

| 用途 | 说明 |
|---|---|
| **CI / 上线前门禁** | `validate` 真实编译 + 沙箱静态扫描 + 冒烟执行。不合格**退出码 2**，可直接 `set -e` 卡流水线 |
| **以审计日志为唯一事实来源的视图** | `status` / `history` 直接读 `docs/script-versions.md`，零副作用 |
| **回滚一致性校验** | `rollback` 会拿给定源码的 checksum 与审计日志记录的"上一个版本"比对，**不一致直接拒绝（退出码 2）** —— 回滚到错的代码比不回滚更危险 |
| **本地复现 / 教学** | 五个阶段（Prepare→Validate→Activate→Rollback→Version）可逐条手工跑，观察每一步的产出 |

---

## 用法

```bash
# 只看状态（读审计日志，零副作用）
bin/scriptctl status
bin/scriptctl history --name damage_formula

# CI 门禁：脚本不合法就退出码 2
bin/scriptctl validate --script scripts/damage_formula.lua

# 上线前彩排：打印计划，不激活
bin/scriptctl reload --script scripts/damage_formula.lua

# 真正激活（仅本进程 VM）
bin/scriptctl reload --script scripts/damage_formula.lua --commit

# 回滚计划：源码必须 == 审计日志记录的上一版（checksum 比对）
bin/scriptctl rollback --name damage_formula --script scripts/damage_formula.v1.lua
```

### 选项

| 选项 | 说明 |
|---|---|
| `--script <path>` | 脚本源码文件 |
| `--name <name>` | 脚本名；缺省取文件 basename 去掉扩展名 |
| `--audit <path>` | 审计日志路径，默认 `docs/script-versions.md` |
| `--commit` | 真正执行激活；缺省只打印计划（dry-run） |
| `--no-smoke` | 跳过冒烟执行，仅做语法 + 沙箱静态扫描。见下 |
| `--help` | 用法（退出码 0） |

### 退出码（刻意区分，禁止一律 0）

| 码 | 含义 | 调用方应对 |
|---|---|---|
| `0` | 通过 / 成功 | 继续 |
| `1` | 用法错误、文件缺失、VM 创建失败等**内部错误** | 查环境 / 修命令 |
| `2` | **脚本被判定不合格**（校验失败 / 回滚 checksum 不一致） | **阻断上线** |

### `--no-smoke` 用在哪

校验分三关：语法编译 → 沙箱静态扫描（禁用 API 名单）→ **冒烟执行**（在隔离临时 VM 里调
`__hot_smoke`）。

`scriptctl` 没有宿主的绑定面（`entity.*` / `skill.*` / `quest.*` …），所以一个**真实游戏脚本**
在冒烟阶段很可能因为 `attempt to index a nil value` 被**误判**为不合格 —— 它引用的绑定在这个
工具里压根不存在。此时用 `--no-smoke`：跳过冒烟，但**静态扫描照常执行**。

实现上不是"关掉 Validate"，而是把 `HotReloader::Config::smoke_function` 置空 ——
`SmokeCall` 遇到空函数名直接判通过（见 `src/hot_reload/hot_reloader.cpp`），
静态扫描与预算检查一行没少。

---

## 实测样例

```
$ bin/scriptctl status
status: audit=docs/script-versions.md scripts=1 records=2
  scene_damage_formula         current=v2    checksum=8b4468c4ec471e11 at_ms=1789036010374 by=hot_reloader records=2

$ bin/scriptctl history --name scene_damage_formula
history: name=scene_damage_formula records=2 (新 → 旧)
  v2    checksum=8b4468c4ec471e11 at_ms=1789036010374 by=hot_reloader
  v1    checksum=8caacc0151d7ff55 at_ms=1789036010374 by=hot_reloader

$ bin/scriptctl validate --script bad_syntax.lua ; echo $?
prepare failed: INVALID_ARGUMENT: CompileError bad_syntax:1: <name> or '...' expected near 'end'
report: ok=0 syntax=0 sandbox=0 smoke=0 smoke_calls=0 elapsed_ms=0 issues=0
validate: REJECTED name=bad_syntax
2

$ bin/scriptctl validate --script banned_api.lua ; echo $?
report: ok=0 syntax=1 sandbox=0 smoke=1 smoke_calls=1 elapsed_ms=0 issues=1
  issue: sandbox: banned api 'io'
validate: REJECTED name=banned_api
2

$ bin/scriptctl rollback --name scene_damage_formula --script damage_formula.lua --no-smoke ; echo $?
rollback: REFUSED checksum mismatch — given source=df5eb5ff622f031f, audit previous(v1)=8caacc0151d7ff55
2
```

---

## 与库内 API 的对应关系

| 子命令 | 调用的库 API |
|---|---|
| `validate` | `HotReloader::PrepareFromFile` + `Validate` |
| `reload` | 同上 + `BeginSafePoint` / `Activate` / `EndSafePoint` |
| `rollback` | 同上 + 审计日志解析 + checksum 一致性硬校验 |
| `history` / `status` | 仅解析 `MarkdownAuditSink` 写出的 Markdown 表格 |

`rollback` **不**直接调 `HotReloader::Rollback()`：后者要求本进程内有该脚本的版本历史，
而一次性 CLI 进程没有历史（历史在 `docs/script-versions.md` 里，不在内存里）。
所以 `--commit` 的实际语义是"**把上一个版本的源码在本进程内重新激活**"，
而**在线进程的回滚**必须由宿主在安全点内调 `HotReloader::Rollback()`
（实现见 `src/hot_reload/hot_reloader.cpp`，选版本时会跳过 checksum 与当前相同的版本，
避免"回滚到同一个版本"白增版本号）。
