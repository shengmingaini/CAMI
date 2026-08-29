# 引擎中立架构纪律（Engine-Agnostic Architecture）

> 目标：**保证客户端渲染引擎（Godot / Unity / 其他）可在后期自由替换，不返工游戏核心。**
> 本文是与 `mmorpg_tasks/docs/rfc/client-engine-selection.md` 配套的架构红线。当前已批准路线为 Godot 4.7.1，但本文件约束的是"无论最终选哪个引擎都要遵守"的纪律。

---

## 1. 接缝模型（Seam）

项目天然分三层，切换引擎的"接缝"在层 2 与层 3 之间：

```
┌─────────────────────────────────────────────┐
│ 层 1：服务端（C++）        —— 引擎无关，已就绪 │
│   Gateway / GameNode / DataService / Control  │
├─────────────────────────────────────────────┤
│ 层 2：游戏核心（Lua 规则 + C++ Simulation）    │
│   —— 引擎无关：战斗公式 / 技能 / 任务 / 经济   │
│   —— 权威状态只在此层（State Ownership）       │
├═══════════════════════════════════════════════┤  ← 接缝（替换点）
│ 层 3：客户端表现层（引擎相关）                 │
│   —— 仅做：渲染 / 输入 / 网络适配 / 预测插值  │
│   —— GDScript(C#) / 场景树 / prefab / 渲染器   │
└─────────────────────────────────────────────┘
```

**核心原则**：层 3 是"薄表现层"，不持有任何权威游戏状态。换引擎 = 重写层 3，层 1 / 层 2 完全不动。

---

## 2. 切换引擎时：什么变、什么不变

| 不变（核心资产，一次投入长期复用） | 变（表现层，切换时重写） |
|---|---|
| 服务端全部 C++ 代码 | 客户端工程（GDScript / C#、场景树 / prefab） |
| Lua 游戏规则脚本（Quest / Skill / Buff 公式 / NPC AI） | 资源导入管线（.tscn / .prefab 等引擎专属格式） |
| FlatBuffers / Protobuf schema（协议契约） | 渲染配置（Godot 渲染器 / Unity URP） |
| 数值与内容配置（JSON / CSV / FlatBuffers） | 引擎专属 UI 实现（Control / UGUI） |
| 美术**源资产**（glTF / FBX 中立格式） | 物理 / 动画绑定（部分引擎相关） |

> 关键：美术源资产用 **glTF / FBX 中立格式**入库；引擎专属格式（.tscn / .prefab）只作为"导入产物"，可随时从源资产 + 导入配置重建。

---

## 3. 引擎中立纪律（Do / Don't）

### Do（必须遵守）

1. **游戏核心逻辑写在 Lua + C++ Simulation**，不写进任何客户端脚本（GDScript / C#）。战斗公式、技能、任务进度、经济规则都是引擎无关资产。
2. **客户端-服务端协议用 FlatBuffers**，schema 独立目录、冻结、带版本号（`--gen` 产出各引擎 binding）。这是切换引擎的契约基础。
3. **数值 / 内容配置用中立格式**（JSON / CSV / FlatBuffers），不用引擎专属 SerializedObject / Resource 当规则真理来源。
4. **美术源资产用 glTF / FBX**；引擎专属资产只作导入产物，保留源 + 导入配置可重建。
5. **客户端严格目录分层**（`runtime / network / gameplay / ui / extensions`），`network` 与 `gameplay` 表现层是替换接缝（见 RFC §6.1）。
6. **权威状态只在服务端**；客户端只做预测、插值、表现（符合总规范 §10 / §12 / §33）。

### Don't（红线，违反即绑死引擎）

1. ❌ 把游戏规则写进 Unity `ScriptableObject` / Godot `Resource` 当真理来源。
2. ❌ 在客户端脚本里硬编码战斗结算（必须服务端权威 + Lua 规则）。
3. ❌ 让客户端直接读 MySQL / Redis（总规范红线，与引擎无关）。
4. ❌ 用引擎专属网络抽象替换统一 Command / Query / Event 信封（总规范 §4 / §36）。
5. ❌ 把美术资产以引擎专属二进制直接入库（应保留 glTF 源 + 导入配置）。

---

## 4. 当前阶段（服务端 + 核心）执行指引

用户决策（2026-08-29）：**暂不安装 / 实现任何客户端引擎，先搭建服务端和游戏核心**，保证后期自由切换。

- ✅ 按 `TASK-001` ~ `TASK-041` 推进服务端。
- ✅ 游戏核心（Lua 规则层、Combat / Skill / Quest / Economy 模块）按规划实现，**全程引擎中立**。
- ⏸️ 客户端任务（TASK-034 / 035 / 036 + 新增 15–20 个）**标记延后**，待引擎确定 / 需要表现层时再实现（RFC §7.2-1 标注）。
- 🔒 **现在就必须冻结 FlatBuffers 协议 schema**——这是切换引擎的契约基础，越早定越省后期返工。**（已落地：`protocol/flatbuffers/`，VERSION 1.0.0，含 `schema/*.fbs` + README 冻结策略，见 `protocol/flatbuffers/README.md`）**。
- 🔒 任何 gameplay 代码评审时，对照 §3 红线检查"是否引入了引擎专属依赖"。

---

## 5. 后期切换引擎时的动作清单

1. 选定引擎（Godot / Unity / 其他）→ 按 RFC §6 的对应子方案重写客户端工程（层 3）。
2. 用目标引擎的 FlatBuffers 代码生成器产出协议 binding。
3. 导入 glTF 源资产，配置该引擎的导入管线（生成 .tscn / .prefab）。
4. 实现客户端 `network / gameplay / ui` 三层，对接 Command / Query / Event 信封。
5. 跑 `TASK-041` 集成回归：客户端连真实服务端冒烟，验证层 1 / 层 2 完全未动。

---

## 6. 自检（CI 门禁建议）

在 `scripts/verify/*.sh` 中加静态扫描：
- 客户端 `network` / `gameplay` 层不得 `import` 引擎专属的规则数据源（如 `.tres` 中的规则表）——规则只能来自 Lua / FlatBuffers / JSON。
- FlatBuffers schema 目录不得被客户端逻辑反向修改（契约单向）。
- 美术源目录必须存在 `.gltf` / `.fbx` 源，引擎专属格式不得作为唯一副本。

---

*关联文档：`mmorpg_tasks/docs/rfc/client-engine-selection.md`（引擎选型）、`docs/game-completion-roadmap.md`（缺口与阶段）、项目总规范 V1.0（§4 通信模型 / §10 State Ownership / §30 Lua / §36 消息信封）。*
