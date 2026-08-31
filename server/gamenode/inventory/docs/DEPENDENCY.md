# Inventory / Equipment — 依赖与模块边界（TASK-017）

## 依赖方向（§27.3）
```
server/gamenode/inventory
  ├─ engine/core        error / bus（EventBus、Result、Error）
  └─ server/gamenode/role    RoleSystem、Character、AttributeSet（装备属性联动，§27.2）
```
- 消费 `role` 仅经公开接口：`RoleSystem::FindByPlayer` / `RecomputeAttributes`，以及
  `Character::attrs.from_equipment`（均为 TASK-016 公开成员）。**禁止访问 role 内部数据**。
- 消费 `core` 仅经公开头（error / bus / time / sched / memory / arena）。

## 被依赖（下游）
- 后续任务（Combat TASK-022、Buff TASK-023、复制 TASK-030）经本模块 `include/` 公开接口调用。
- 禁止下游 `#include` 本模块 `src/` 或内部头（验收脚本静态扫描 `include/` 是否泄露 `src/`）。

## 红线
- 不引入 MySQL / Redis / gRPC / 网络；落盘只经 `RoleSystem` 的 `IPersistenceAdapter` 异步投递。
- 不依赖 `core::ConfigManager` 全局快照（多文件同键合并冲突）；物品表用 `ItemDefStore` 受限解析。
- 无循环依赖；新增同类能力走注册表/ID 段，禁止 `switch` 硬编码穷举。

## 构建
`server/gamenode/CMakeLists.txt` 已 `add_subdirectory(inventory)`。目标
`mmo_gamenode_inventory`（别名 `mmo::gamenode_inventory`），链 `core_error/core_bus/gamenode_role`。
测试 `inventory_test` → ctest `Inventory.Suite`；bench `inventory_bench` → `bin/inventory_bench`。
