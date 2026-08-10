# 协议设计验收核对（对照 2026-08-12 任务）

> **核对日期**: 2026-08-12（按用户要求回溯 Day 3 协议交付物）  
> **被核对对象**: Day 3（2026-08-07）协议基础交付  
> **关联文件**: `docs/protocols/protocol-spec.md` / `README.md` / `proto/flatbuffers/*.fbs` / `proto/protobuf/*.proto`

---

## 0. 任务验收标准（来自 2026-08-12 任务卡）

- **任务**: 协议 IDL 定义：FlatBuffers(高频) + Protobuf(低频)、协议清单 v1
- **交付物**: `protocol.fbs`、`protocol.proto`、`协议清单v1`
- **验收**: ① 高频协议零 JSON；② 清单含版本号字段

## 1. 核对结论（速览）

| 验收项 | 状态 | 说明 |
|--------|------|------|
| 高频协议零 JSON | ✅ 设计层达标 | 轨道 A 自定义位压缩、轨道 B FlatBuffers，spec §1 硬性禁令禁 JSON |
| 清单含版本号字段 | ❌ 未达标 | 现有清单（README 索引 + spec §5 目录）无逐条版本列；`MessageEnvelope` 也无版本字段（仅 ClientHello 带 `proto_version`） |
| 交付物 `protocol.fbs` | ❌ 命名不符 | Day 3 交付为**拆分多文件**（common/login/movement/combat/aoi/envelope.fbs），无单文件 `protocol.fbs` |
| 交付物 `protocol.proto` | ❌ 命名不符 | Day 3 交付为 common/cross_server/config_*.proto，无单文件 `protocol.proto` |
| 交付物 `协议清单v1` | ❌ 命名+内容均不符 | 无此名文件；现有协议目录**显式声明不完整**（缺社交/任务/经济/背包/场景/寻路/跨服域） |

**总评：Day 3 已覆盖本任务的"实质内容"（双轨 IDL + 零 JSON + 部分清单），但按 2026-08-12 任务卡的"字面交付物与验收项"判定，未完成。** 存在 3 个命名/结构偏差 + 1 个内容缺口。

---

## 2. 逐项证据

### 2.1 高频协议零 JSON — ✅ 达标（设计层）
- `protocol-spec.md` §1 硬性禁令第 1 条：*禁止 JSON 传高频战斗数据 → 全部走二进制（FlatBuffers / 位压缩）*。
- 轨道 A（高频）：`protocol-spec.md` §3/§7 明确为**自定义位压缩**（非 FlatBuffers、非 JSON），移动增量 11B。
- 轨道 B（可靠）：全部 FlatBuffers 二进制（`proto/flatbuffers/*.fbs`）。
- 配置/跨服（低频）：Protobuf 二进制（`proto/protobuf/*.proto`）。
- ⚠️ 注意：**环境无 flatc/protoc**，所有 schema 为人工校对契约，未编译校验（与 Day 3 同源缺口，非本次新增）。

### 2.2 清单含版本号字段 — ❌ 未达标
- `docs/protocols/README.md` §1 清单表头为 `文件 | 类型 | 说明`，**无版本列**。
- `protocol-spec.md` §5 消息目录表头为 `域 | 逻辑消息 | 轨道B类型 | 轨道A opcode`，**无版本列**。
- 线结构：`MessageEnvelope`（envelope.fbs）字段为 `seq/request_id/flags/body`，**无 version 字段**；仅 `login.fbs` 的 `ClientHello` 带 `proto_version: uint16` + `client_version: string`（握手层版本，非清单/信封版本）。
- 配置表有 `data_version`（`config_*.proto` 的 `*ConfigSet`），但那是配置数据版本，非协议清单版本。

### 2.3 交付物 `protocol.fbs` / `protocol.proto` — ❌ 命名不符
- 实际 FlatBuffers 文件（6 个）：`common.fbs` `login.fbs` `movement.fbs` `combat.fbs` `aoi.fbs` `envelope.fbs`。
- 实际 Protobuf 文件（5 个）：`common.proto` `cross_server.proto` `config_items.proto` `config_skills.proto` `config_quests.proto`。
- **无单一聚合的 `protocol.fbs` / `protocol.proto`**。

### 2.4 交付物 `协议清单v1` — ❌ 命名 + 内容均不符
- 无名为 `协议清单v1`（或 protocol-manifest）的文件。
- 现有"清单"分散在 `README.md`（schema 索引）与 `protocol-spec.md` §5（消息目录），二者均未标 "v1" 且**目录显式声明不完整**：
  > spec §5 末尾：*「其余域（社交/任务/经济/背包/场景/寻路/跨服）的消息在对应模块详细设计阶段补充到本目录」*
- 而 quest/economy/social 模块文档中引用的 `quest.fbs`/`economy.fbs`/`social.fbs` 均标注"(待建)"——即这些域的 IDL 尚未落地，**v1 完整清单无法在 Day 3 收口**。

---

## 3. 缺口清单（需决策后再补）

| 编号 | 缺口 | 性质 | 建议处置 |
|------|------|------|----------|
| G1 | 无 `protocol.fbs`/`protocol.proto` 单文件 | 命名/结构 | **建议保留拆分**（按域拆分对 5万CCU 项目更优，单文件是反模式）；改为新建"协议清单v1"引用这些拆分文件。若坚持单文件，则需合并（不推荐）。 |
| G2 | 清单无逐条版本字段 | 验收缺口 | 在清单增加 `版本` 列（各 schema 当前均为 v1.0.0），并给 `MessageEnvelope` 加 `proto_version: uint16` 字段以对齐握手版本。 |
| G3 | 无 `协议清单v1` 文件 | 命名 | 新建 `docs/protocols/protocol-manifest-v1.md`，列出全部 .fbs/.proto + 版本 + 域归属 + 状态（含待建）。 |
| G4 | 协议目录不完整（缺 social/quest/economy 等域） | 内容 | 要么在 v1 清单中**显式标记 pending**（推荐，符合 Day 3 分期策略），要么把待建 .fbs 一并补齐（超出 Day 3 范围，工作量更大）。 |

---

## 4. 建议下一步（需用户拍板）

- **选项 A（最小闭环，推荐）**：保留拆分 IDL，新建 `协议清单v1`（含版本列 + 状态列，pending 域显式标注）+ 给 `MessageEnvelope` 补 `proto_version` 字段。约 1 个文档 + 1 处 fbs 改动，即可让 2026-08-12 验收"字面达标"。
- **选项 B（强一致）**：在 A 基础上，把 quest/economy/social 三个待建 .fbs 也产出，使 v1 清单真正完整（工作量显著增大，且涉及模块协议设计，建议独立排期）。
- **选项 C（按字面交付）**：合并为单文件 `protocol.fbs`/`protocol.proto`（**不推荐**，违背按域拆分的可维护性，且 union/include 结构需重构）。

> 结论先讲清楚：**Day 3 没"完成"这个 2026-08-12 任务卡**——它完成了地基，但交付物命名、版本字段、清单完整性三项验收都没过。这是好事，现在补比编码阶段返工便宜。
