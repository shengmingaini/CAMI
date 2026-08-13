# 内容模块化与分阶段加载设计（Content Modularization & Phase-Gated Loading）

> **版本**: v1.0 | **日期**: 2026-08-12
> **目标**: 玩法内容（宝石/附魔/天赋/技能/等级上限等）模块化 + 按需加载，最小化初期运行容量。
> **原则**: 数值全配置驱动（禁止硬编码）、玩法全 Lua 化（C++ 不写玩法逻辑）、模块通信走事件总线。

---

## 1. 问题与现状

- 玩法系统全量加载 = 启动即解析全部 21 个 ConfigSet + require 全部玩法 Lua 模块 + 预分配全部对象池——初期几十人在线，却养着全部玩法系统的内存/CPU。
- 现状盘点：
  - 机制层 21 个 `proto/protobuf/config_*.proto` 已定义（含 gems/enchantments/skills/progression）；
  - `verify/load_configs.py` 仅注册 13 个核心 ConfigSet（验证用），**玩法级配置未接入任何运行时加载器**；
  - `lua/` 仅骨架（config/skills/quests 空目录）；配置中心（configs/）未实现；
  - **结论**：现在正是建立"内容模块体系"的最佳窗口——先立骨架，后续玩法实现直接按模块落位。

## 2. 内容分层模型（Content Tiers）

| Tier | 语义 | 加载时机 | 模块示例 |
|------|------|---------|---------|
| **Tier 0** | 核心常驻 | 服务器启动 | 角色属性/背包/货币/基础战斗/技能执行器骨架/任务引擎/AOI/区域刷新 |
| **Tier 1** | 玩家等级解锁 | 首玩家达等级门槛 | 技能树数据/天赋/装备属性/物品使用 |
| **Tier 2** | 进阶玩法 | 服务器 phase 开启 + 等级达标 | 宝石/附魔/专业/PVP/声望/世界事件 |
| **Tier 3** | 扩展内容 | 发布周期开启 | 坐骑宠物进阶/家园/大型副本 |

**双闸门模型**：模块可见 = 服务器 phase 启用 ∧ 玩家等级/前置条件达标。
服务器闸门控制"内容是否上线"（灰度/资料片），玩家闸门控制"个人是否解锁"（WoW 式成长）。

## 3. 内容模块注册表（ContentModuleRegistry）

每个玩法 = 一个**内容模块**，静态声明（配置/代码），运行时由加载器驱动：

```cpp
// game/content/content_module.h — 注册表项（纯声明，无玩法逻辑）
struct ContentModule {
    std::string  id;                       // "gems"
    int          tier;                     // 2
    std::vector<std::string> deps;         // {"items", "stats"}  依赖模块先加载
    std::vector<std::string> config_sets;  // {"config_gems"}      对应 proto ConfigSet
    std::string  lua_entry;                // "systems/gems/init.lua"
    int          level_gate;               // 20                    玩家等级门槛
    std::string  phase;                    // "release"             服务器阶段门槛
    std::size_t  pool_entities;            // 1024                  对象池预分配规格
};
```

- 注册表 = `std::unordered_map<std::string, ContentModule>`，静态初始化（零运行时开销）；
- 加载器按 `deps` 拓扑排序，先加载依赖再加载自身；
- 模块激活后注册到 EventBus，与其余模块仅通过事件交互（红线合规）。

## 4. 阶段解锁（Phase Gates）

### 4.1 服务器阶段（灰度/内容包开关）

配置中心 `configs/content_phase.json`：

```json
{
  "phase": "alpha",
  "level_cap": 30,
  "enabled_modules": ["skills", "talents", "items"],
  "disabled_modules": ["gems", "enchanting", "professions", "pvp", "reputation", "world_events"]
}
```

- **等级上限分阶段**：alpha=30 / beta=60 / release=70——同一套 `config_progression` 数据，只切 level_cap 配置；
- phase 热切换 = 灰度开放新玩法（Lua 热更路径已支持，无需重启）；
- 阶段推进（alpha→beta→release）仅改配置中心，代码零改动。

### 4.2 玩家闸门（成长解锁）

- 模块级 `level_gate`（如 gems=20 级）+ 可选任务/声望前置（WoW 式）；
- 玩家升级时触发 `ModuleUnlockCheck` 事件 → ContentRegistry 懒加载 → 该玩家可见新玩法；
- 前置配置（解锁提示/新手引导）也由模块自身携带，随模块激活。

## 5. 懒加载机制（Lazy Loading）

### 5.1 配置层（ConfigManager）

- 启动只加载 Tier 0 模块的 ConfigSet；
- 模块激活时 `ensure_loaded(config_set)`——按需解析 `data/configs/*.json`（现有 `json_format.Parse` 复用）；
- 配置缓存带 **LRU + 容量上限**（默认 64MB）与引用计数，长期无引用的模块配置可卸载（模块卸载时）；
- 每个 ConfigSet 仍是独立热更单元（现有语义保留）。

### 5.2 Lua 层（LuaModuleLoader）

- 每个玩法一个 Lua 模块，启动只 `require` Tier 0 核心模块；
- 模块激活时 require（Lua require 自带缓存，二次触发零解析成本）；
- 玩法脚本激活后驻留，热更走现有 Lua 热更通道（按模块替换，不影响其他模块）。

### 5.3 C++ 层（Service 懒汉）

- 玩法 Service 对象（如 GemService）按需创建（`std::once_flag` 懒汉单例），注册进模块容器；
- **对象池只预分配已激活模块的实体**——未激活模块的池容量不占内存（红线"禁止运行时动态申请"对已激活模块仍然成立，容量来自注册表 `pool_entities`）。

## 6. 模块归属清单（现有 21 配置 → Tier 映射）

| 配置集（proto） | Tier | 模块 | 说明 |
|----------------|------|------|------|
| config_stats / config_currencies / config_balance | 0 | 核心 | 启动加载 |
| config_items / config_creatures / config_zones / config_spawns | 0 | 核心（世界基础） | 启动加载 |
| config_quests | 0 | 任务引擎 | 启动加载 |
| config_skills | 1 | skills | 技能树数据 |
| config_progression | 1 | progression | 等级曲线/等级上限（phase 切换） |
| config_sets / config_buffs | 1 | talents/buffs | 天赋与增益 |
| config_gems | 2 | gems | 宝石（用户点名） |
| config_enchantments | 2 | enchanting | 附魔（用户点名） |
| config_professions / config_vendors | 2 | professions/vendors | 专业/商人 |
| config_pvp / config_reputation / config_world_events | 2 | pvp/reputation/world_events | 进阶玩法 |
| config_loot | 2 | loot | 掉落表（随副本/PVP 激活） |

## 7. 与架构红线对齐（合规声明）

- ✅ 禁止单体架构：模块独立声明、仅经 EventBus 交互、可独立热更/独立禁用；
- ✅ Lua 与 C++ 严格解耦：C++ 侧只有注册表+加载器（骨架），玩法逻辑全 Lua；
- ✅ 数值/条件全配置驱动：模块归属/Tier/闸门全在配置，禁硬编码；
- ✅ 无全局锁/单点：注册表只读（静态），激活路径 `std::once_flag` 无锁热路径；
- ✅ 事件总线：解锁事件/激活事件走 EventBus，不跨模块直调。

## 8. 收益预估

| 指标 | 全量加载（现状趋势） | 模块化懒加载 | 提升 |
|------|--------------------|-------------|------|
| 启动加载 ConfigSet | 21 个 | ~7 个（Tier 0） | **↓ 67%** |
| 启动 require Lua 模块 | 全部玩法 | 仅核心 | 内存 ↓ 60-70% |
| 配置缓存常驻 | ~50MB（估算） | ~15MB 起步，按需增长 | **↓ 70% 起步** |
| 对象池预分配 | 全部模块 | 已激活模块 | 按激活数线性 |
| 启动时间（配置+Lua 编译） | ~5s | ~1.5s | **↓ 70%** |
| 新玩法上线 | 全量发布/重启 | 配置中心 phase 热切换 | 质变 |

## 9. 实施路径（待办）

| # | 任务 | 优先级 | 说明 |
|---|------|--------|------|
| 1 | `game/content/content_module.h` 注册表 + `content_registry.cpp` 加载器骨架 | P0 | 声明式模块 + 依赖拓扑排序 + ensure_loaded |
| 2 | `ConfigManager` 懒加载（复用 json_format.Parse）+ LRU 缓存 | P0 | 与 `verify/load_configs.py` 语义对齐 |
| 3 | `LuaModuleLoader` 懒 require + 模块级热更 | P0 | 与 Lua 热更模块对接 |
| 4 | `configs/content_phase.json` 阶段配置 + level_cap 切换 | P1 | 等级上限分阶段落地 |
| 5 | 模块解锁事件（升级→ModuleUnlockCheck→懒加载） | P1 | EventBus 集成 |
| 6 | 对象池按模块预分配（pool_entities 驱动） | P1 | 与游戏层对象池对接 |
| 7 | 现有玩法实现按模块落位（skills→Tier1 等） | P2 | 随玩法开发推进 |

> **依赖关系**：本设计不依赖 Scale-to-Fit 部署档位（docs/architecture/scale-to-fit.md），两者正交——
> 前者管"内容加载范围"，后者管"中间件规模"；配合使用即"最小进程 + 最小内容"双小起步。
