# Day 3 方案B — 配置表 Protobuf Schema 交付说明

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-07 (Day 3 — 方案 B：配置表)  
> **上游**: `docs/protocols/protocol-spec.md` §12（"配置表 Protobuf schema 在本规约 `proto/protobuf/` 下扩展"）

---

## 1. 一句话定位

在 `proto/protobuf/` 下补齐**物品 / 技能 / 任务**三类**数据驱动静态配置**的 Protobuf schema，作为数据层加载、模块（character/combat/quest）读取的契约。**数值逻辑（伤害公式等）仍由 Lua 热更边界承担**，`schema` 只放静态参数 + 对脚本的引用（`script_ref`）。

## 2. 文件清单

| 文件 | 包 | 关键 message / 枚举 |
|------|----|---------------------|
| `common.proto`（扩展） | `CAMI.Common` | `ClassType` / `StatType` / `StatModifier` / `EffectType` / `EffectConfig` / `ItemReward` |
| `config_items.proto` | `CAMI.Config` | `ItemConfig` / `ItemConfigSet`；`ItemType` / `ItemQuality` / `BindType` / `EquipSlot` |
| `config_skills.proto` | `CAMI.Config` | `SkillConfig` / `SkillConfigSet`；`SkillType` / `TargetType` |
| `config_quests.proto` | `CAMI.Config` | `QuestConfig` / `QuestConfigSet` + `QuestObjective` / `QuestReward`；`QuestType` / `ObjectiveType` |

## 3. 设计要点

- **共享类型进 `common.proto`**：`StatType` / `StatModifier` / `EffectConfig` / `ItemReward` / `ClassType` 跨三类配置复用，避免重复定义（common.proto 注释已声明"被各配置表 proto 复用"）。
- **技能数值不进 schema**：`SkillConfig` 放静态参数（冷却 / 施法时间 / gcd / 法力 / 射程 / 半径 / 最大目标数）+ `base_value` / `coefficient` / `scaling_stat` 作为 fallback，并附 `script_ref` 指向 Lua 脚本。实际伤害结算由 combat.md §1.2 的 Lua 边界产出。
- **每个文件一个 `*ConfigSet` 容器**（含 `data_version`）作为整体加载 / 热更单元，配合运维层 config-center。
- **i18n 不内联**：`name_key` / `description_key` 引用语言包，schema 不存展示文本。
- **命名与 Round 1 一致**：proto3 / `snake_case` 字段 / `PascalCase` message / `package CAMI.*`（同 `CAMI.Common` / `CAMI.CrossServer` 风格）。

## 4. 已同步更新

- `docs/protocols/README.md`：索引表追加三个配置 proto；待办项"配置表 Protobuf schema"勾选为已完成。
- `docs/modules/character.md` §11：开放问题"配置表 Protobuf schema 由方案 B 阶段补充"标注为 ✅ 已完成。

## 5. 待办（编码阶段）

- 环境无 `protoc`，schema 为人工校对契约；装 `protoc` 后编译校验：
  ```bash
  protoc --cpp_out=gen/pb proto/protobuf/common.proto \
         proto/protobuf/config_items.proto \
         proto/protobuf/config_skills.proto \
         proto/protobuf/config_quests.proto
  ```
- `quest` / `economy` / `social` 等模块详细设计尚未写，其配置引用待落地。
- 后续补充更多配置表（怪物 / NPC / 商店 / 成就 / 天赋树等）时，沿用本结构：`<Domain>Config` + `<Domain>ConfigSet`，共享类型复用 `common.proto`。
