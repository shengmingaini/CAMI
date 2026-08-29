# 跨服务协议契约（FlatBuffers，冻结）

> **定位**：本目录是整个 MMORPG 项目的**引擎中立协议基石**，对应 `docs/engine-agnostic-architecture.md` §4 的"现在就必须冻结 FlatBuffers 协议 schema"要求。
>
> 切换客户端渲染引擎（Godot / Unity / 其他）时，**本目录一字不改**——它定义的是"服务端与客户端之间传什么"，不关心谁在渲染。换引擎 = 用目标引擎的 flatc 重新生成 binding，核心资产零返工。

---

## 1. 当前版本

`VERSION` = **1.0.0**（见 `./VERSION`）

语义化版本：`MAJOR.MINOR.PATCH`
- **PATCH** 文档/注释修正，schema 二进制兼容
- **MINOR** 纯增量扩展（新表 / 新联合成员 / 新可选字段，详见 §3）
- **MAJOR** 破坏性变更（需双版本并行支持窗口）

---

## 2. 冻结策略（红线）

| 规则 | 说明 |
|---|---|
| **已发布的字段 / 表 / 枚举值不可变** | 一旦随某个 MINOR/MAJOR 发布，其 `id`、类型、语义永久冻结 |
| **禁止删除 / 重排字段** | 需要下线用 `deprecated` 属性标记，物理保留占位移除逻辑 |
| **禁止复用已弃用的 `id`** | 弃用槽位永久保留，避免老客户端误读 |
| **扩展只走"追加"** | 新增命令/查询/事件 = 追加枚举值 + 追加联合成员 + 追加对应表 |
| **信封永远单一套** | 任何模块不得在 Envelope 之外另起消息头（总规范 §36） |
| **改 schema 必须 RFC + 版本 bump** | 直接改 `schema/*.fbs` 的 PR 若无 RFC 引用与 VERSION bump，CI 拒绝合并 |

---

## 3. 如何增量扩展（additive-only 范例）

以"新增一个 `TELEPORT` 命令"为例，零破坏现有契约：

1. `command.fbs` 枚举追加：`TELEPORT = 7,`（不插队、不删旧值）
2. `command.fbs` 联合追加：`TeleportCmd,`
3. `command.fbs` 新增表：
   ```flatbuffers
   table TeleportCmd {
     dest_scene:uint32;
     dest_pos:Vec3;
   }
   ```
4. `VERSION` bump 到 `1.1.0`
5. 提交时引用对应 RFC / 任务编号

老客户端读到未知 `CommandKind` 时按 `UNKNOWN` 安全丢弃，不崩溃——这是 FlatBuffers 前向兼容的天然保证。

---

## 4. 文件结构

```
protocol/flatbuffers/
├── VERSION                 # 当前 schema 版本（1.0.0）
├── README.md               # 本文件（冻结策略 + 生成指南）
├── .gitignore              # 忽略 generated/
├── schema/                 # ★ 冻结源（PR 受 §2 红线约束）
│   ├── common.fbs          # 公共原子类型（Vec3 / EntityRef / ID 别名）
│   ├── envelope.fbs        # 统一信封（总规范 §36）+ 顶层联合
│   ├── command.fbs         # Command 基 + CommandKind + CommandBody 联合
│   ├── query.fbs           # Query 基 + QueryKind + QueryBody 联合
│   └── event.fbs           # Event 基 + EventKind + EventBody 联合
└── generated/              # flatc 产出（git 忽略，按目标语言生成）
```

---

## 5. 代码生成（flatc）

> 本项目已内置本地编译器 **`tools/flatc/flatc.exe`（v25.12.19，Windows 预编译，2026-08-29 下载验证）**，可直接调用，无需另行安装。下方命令已用本地路径为例；CI/其他平台请替换为对应 `flatc`。

**服务端（C++，已就绪）**
```bash
flatc --cpp -o generated/cpp schema/*.fbs
# 一次性传入全部 schema：flatc 自动解析 include 图，生成 5 个 _generated.h
# （common / command / query / event / envelope）。切勿只传 envelope.fbs，
# 否则只会生成 envelope_generated.h，其余 4 个被 include 的头缺失，C++ 编译断链。
```

**后期客户端（按所选引擎，切换时不改 schema）**
```bash
# Godot 路线：C++ GDExtension 复用同一套 --cpp 产物
flatc --cpp -o generated/gdextension schema/*.fbs

# Unity 路线（可选回切）：C# binding
flatc --csharp -o generated/csharp schema/*.fbs
```

**验证状态（2026-08-29）**：本地 `tools/flatc/flatc.exe`（v25.12.19，Windows 预编译）已成功编译全部 schema 并生成 5 个 C++ 头文件，`union MessageBody` 代码生成完整。两点已修正的 FlatBuffers 约束（扩展时勿再犯）：
1. 枚举首值必须为 `0`（`MessageType` 已加 `UNKNOWN = 0`），否则字段默认值 0 不在枚举内会编译失败。
2. `union` 必须声明在引用它的 `table` **之前**（同文件内），否则 `root_type` 解析报 "type referenced but not defined"。

`file_identifier "MSG0"` 已写入 `envelope.fbs`，读端用 `flatbuffers::Verifier` + `file_identifier` 校验，防误读非本协议缓冲区。

---

## 6. CI 门禁（建议）

1. **schema 冻结扫描**：`schema/` 下任何改动必须伴随 VERSION bump + 提交信息含 RFC/任务引用，否则 `verify/freeze_gate.sh` 失败。
2. **契约单向**（来自 `engine-agnostic-architecture.md` §6）：客户端 `network` / `gameplay` 层不得反向修改 `schema/`（契约单向）；静态扫描禁止客户端逻辑写回 `.fbs`。
3. **生成一致性**：每次 CI 用固定版本 flatc 重新生成 `generated/`，与缓存比对，防止手改生成物漂移。

---

## 7. 1.0.0 已契约化内容

| 类别 | 基结构 | 1.0.0 内置示例 |
|---|---|---|
| 信封 | `Envelope`（`MessageType` + `payload` 联合 + 经济附加字段） | — |
| Command | `Command { player_id, kind, body }` | MOVE_PLAYER / CAST_SKILL / EQUIP_ITEM / BUY_ITEM / ACCEPT_QUEST / LEAVE_PARTY |
| Query | `Query { player_id, kind, body }` | GET_PLAYER / GET_INVENTORY / GET_SCENE_INFO / GET_GUILD / GET_AUCTION |
| Event | `Event { source_entity, kind, body }` | PLAYER_MOVED / SKILL_CAST / DAMAGE_APPLIED / QUEST_COMPLETED / ITEM_ADDED / PLAYER_ENTERED_SCENE / PLAYER_DISCONNECTED |

> 示例载荷是 **canonical 参考模板**，后续 TASK（Combat / Skill / Quest / Economy / Social）按 §3 规则追加自己的命令/查询/事件，不破坏 1.0.0 基线。

---

*关联：引擎中立架构 `docs/engine-agnostic-architecture.md` §4（冻结项）、项目总规范 V1.0 §4（通信模型）/ §29（序列化）/ §36（消息信封）。*
