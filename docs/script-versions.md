# 脚本版本审计日志（Script Version Audit Log）

> TASK-032 · Lua Hot Reload —— 每次**成功激活**（含人工回滚与自动回滚）追加一行。
> 由 `HotReloader` 阶段 6（Commit）经 `MarkdownAuditSink` 写出；按
> `(脚本名, 版本, checksum)` 去重，因此**重复运行同一批验收不会改动本文件**。
> 运行期持久化必须经 DataService（§13），本文件是开发期审计视图。

| 脚本名 | 版本 | checksum | 激活时间(ms) | 操作者 |
|---|---|---|---|---|
| scene_damage_formula | 1 | 8caacc0151d7ff55 | 1789036010374 | hot_reloader |
| scene_damage_formula | 2 | 8b4468c4ec471e11 | 1789036010374 | hot_reloader |
