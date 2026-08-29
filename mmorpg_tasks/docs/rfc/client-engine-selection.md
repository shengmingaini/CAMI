# RFC：客户端引擎选型（Unity 6.3 LTS vs Godot 4.7.1）

| 项 | 内容 |
|---|---|
| 状态 | **已批准：Godot 4.7.1 路线（2026-08-29 由 Unity 路线回切）** —— 代理拦 Unity 安装/激活 + 用户选择；TASK-034/035/036 按 §6.1 重写，新增客户端任务按 §6.1 生成 |
| 类型 | Architecture Change Request（客户端渲染架构） |
| 日期 | 2026-08-29（§3 / §4 / §5 / §6 / §7 同日多轮修订；结论见 §4.6） |
| 影响任务 | TASK-034 Client Core、TASK-035 Renderer、TASK-036 Resource/Low Spec、TASK-041 集成回归；另需新增客户端任务 15–20 个 |
| 冲突条款 | TASK-035 原「禁止引入重型第三方引擎」约束**已撤销**；项目总规范 §21（低配）、§35（资源占用）仍待 RFC 子任务修订（见 §7.2） |
| 结论 | **已批准：Godot 4.7.1 + GDScript + GDExtension(C++) + 兼容性/Forward+ 渲染 + 全文本 AI 协作** |

---

## 1. 背景

原任务包在 TASK-035 中隐含锁定了「**自研轻量 D3D11 渲染器**」路线，并明确禁止引入第三方引擎。
但客户端仅分配了 3 个任务（034/035/036），对一个需要 UI、动画、特效、音频、场景流式的 3D MMO 客户端而言严重不足（合理预算 15–20 个任务）。

现评估引入成熟引擎替代自研渲染器，以大幅压缩客户端工作量。

> 注：**更新后的项目总规范 V1.0（§20 客户端模块 / §21 低配 / §35 资源占用）本身是引擎中立的**，并未强制自研渲染器。因此本次变更**不违反总规范**，仅与 TASK-035 的任务级文案冲突，需走 RFC 修订该任务。

---

## 2. 版本与事实基线（2026-08-29 核实）

| | Godot | Unity |
|---|---|---|
| 当前稳定版 | **4.7.1**（2026-07-14） | **6.3 LTS**（6000.3.x，2025-12-04）；更新线 6.5（2026-06）；下个 LTS 为 6.7 |
| 授权 | **MIT**，免费商用、零分成、无席位限制、无需账号 | Personal 免费（营收/融资 < $100k），否则需 Pro；**Runtime Fee 已于 2024-09 取消** |
| 安装形态 | **无安装程序**，ZIP 解压即用，单文件自包含，无注册表 | 需 Unity Hub + 安装程序（约 3.9GB） |

---

## 3. 三维度对比

### 3.1 工作站（开发机）要求

| | Godot 4.7.1 | Unity 6.3 LTS |
|---|---|---|
| 内存 | **4 GB 最低**，建议 8 GB | 官方**最低建议 8 GB**，复杂项目需更多；社区建议 16–32 GB |
| 存储 | **200 MB**（编辑器 ZIP 约 72 MB；导出模板约 1.2 GB） | 安装约 3.9 GB，**建议 10 GB+ 可用空间**，且要求高 IOPS 磁盘 |
| GPU | **Vulkan 1.0 推荐；OpenGL 3.3 / GLES 3.0 最低**（兼容性渲染器） | DX10 / DX11 / DX12 或 Vulkan |
| 开发依赖 | 无（C# 需 .NET SDK 8.0+） | IL2CPP 需 **Visual Studio 2019+（含 C++ 工具）+ Windows SDK 10.0.19041.0+** |
| 启动开销 | 秒开 | 项目导入/编译耗时显著 |

### 3.2 最终客户端（导出产物）要求

| | Godot 4.7.1（官方文档，简单 2D/3D 项目） | Unity 6.3 LTS（Windows 桌面 Player） |
|---|---|---|
| CPU | x86_32(SSE2)/x86_64/ARMv8，例：Core 2 Duo E8200 | x86/x64 SSE2、Arm64 |
| GPU | **兼容性渲染器：OpenGL 3.3 或 Direct3D 11（Windows）**，例：Intel HD 2500<br>Forward+/Mobile：Vulkan 1.0 / D3D12(12_0) / Metal 3 | DX10 / DX11 / DX12 / Vulkan（**不再支持 DX9**） |
| 内存 | **2 GB**（原生导出）／4 GB（Web） | **官方未给出桌面 Player 内存下限** |
| 存储 | **150 MB** | 未给出；实际 URP 构建基线显著高于 Godot |
| 系统 | Windows 10（Win7 对兼容性渲染器 best-effort） | Windows 10 21H1（build 19043）+ |

> **对本项目 Low 档目标（4 核 / 4GB RAM / 1GB VRAM / 传统 DX11 GPU）的判定**：
> - **Godot**：兼容性渲染器官方即支持 DX11，内存下限 2GB —— **目标可达，有官方依据**。
> - **Unity**：官方未公布桌面端低配下限，且无 DX9 回退；在 4GB 总内存约束下运行 URP 3D MMO **基本不可达**，只能被迫上调最低配置，与规范 §21/§35 冲突。

### 3.3 WorkBuddy / AI 协作友好度（本项目核心考量）

| 维度 | Godot | Unity |
|---|---|---|
| 工程文件格式 | **全文本**：`.tscn` / `.tres` / `.gd` / `project.godot` | `.unity` 场景为 YAML + 32 位 GUID 引用，配 `.meta` 伴随文件 |
| AI 直接读写工程文件 | **高**，可直接生成/校验/合并 | 低，GUID 引用易被破坏；**但由 Unity MCP 大幅弥补**（见 §3.4） |
| 官方 AI 助手 | 无（仅社区方案） | **Unity AI**（2026-05-04 公测）：编辑器内、带完整工程上下文 |
| 外部 AI 接入 | 无官方 MCP | **Unity MCP**（MIT，5.8k stars）+ AI Gateway |
| 命令行构建 | `godot --headless --export-release`，**免激活、不联网** | `-batchmode -buildWindows64Player`，**需已激活许可证** |
| 无头 CI 门禁 | **天然契合** `scripts/verify/*.sh` | 需编辑器实例 + 许可证；MCP 依赖 localhost:8080，纯无头较绕 |
| 企业代理环境 | **完全离线可用** | Hub 安装与许可证激活需联网，**代理下有失败风险** |
| 训练数据 / 生态 / 已上线先例 | 中等 | **显著更丰富**（教程、Asset Store、MMO 套件、大量已上线 MMO 先例） |

### 3.4 Unity AI 工具链现状（2026-08 核实）

- **Unity AI**（前身 Unity Muse，Muse 已废弃）：2026-05-04 进入公测，编辑器内嵌，走第三方前沿模型（当前为 Gemini），带完整工程上下文。能力：文本生成 C# 脚本、图像提示搭建场景、结合工程上下文给性能优化建议、生成占位资产。GDC 2026 演示「单提示 20 分钟生成可玩 roguelike 原型」。
  - 局限：不碰版本控制、无法跨 60 文件重构、自定义渲染管线问题帮不上；产出偏「教程同质化」，需人工改造。
  - 成本：Personal 试用后 **$10/月（1000 credits）**；完整能力含于 **Unity Pro $185/月**。
- **Unity MCP**（开源 MIT，5.8k stars）：Python MCP Server → HTTP → 编辑器内 C# 插件（localhost:8080），可对接 Cursor / Claude Desktop / GitHub Copilot / Windsurf，亦可配置为 WorkBuddy 的 MCP connector。25+ 工具：增删改 GameObject、管理场景、**写入并校验 C# 脚本**、**程序化运行单元测试**、批量操作。
  - 这条**实质性削弱了「Unity 工程文件对 AI 不友好」的论点**——外部 Agent 可绕过 YAML/GUID，直接通过编辑器 API 操作工程。
  - 局限：需编辑器运行（localhost:8080），纯无头 CI 场景比 Godot 绕。

> **行业共识（2026）**：AI 能力已不再是引擎间的实质差异化因素，选型应回到渲染、平台支持、社区与资产生态。选型时不应把 AI 工具作为决定性权重。

---

## 4. 建议决策（条件性）

> **修正说明（2026-08-29）**：本 RFC 初版单向推荐 Godot，过度加权了两项：
> ① Godot 官方 2GB / 150MB 是**「简单 2D/3D 项目」**的地板，并非 MMO 地板——真实 MMO 客户端（分区资源、角色模型、UI 图集、音频常驻、网络缓冲、大量同屏实体）在任何引擎上都会显著超出；
> ② Unity MCP 已实质性弥补「工程文件对 AI 不友好」这一弱点。
> 因此在「目标是大型 MMORPG」这一前提下，**Unity 6 应作为首选**。

### 4.1 分水岭：Low 档最低配置是否为硬约束？

| | Low 档为硬约束（4核 / 4GB / 1GB VRAM） | Low 档可上调 |
|---|---|---|
| 选择 | **Godot 4.7.1** | **Unity 6.3 LTS** |
| 依据 | 兼容性渲染器官方支持 DX11，导出产物最低 2GB RAM / 150MB | Addressables 流式加载成熟，是 Godot 已知短板的主战场 |

**但必须明确**：即便选 Godot，4GB / 1GB VRAM 也不是引擎白送的——它取决于**内容预算**（贴图尺寸、面数、同屏实体数、DrawCall、音频常驻量）。规范 §35 的 Low 目标在任何引擎上都属于激进值，需靠预算硬控，且必须实测后写入容量报告，不得未测先宣称兼容。

### 4.2 若目标是「类魔兽世界大型 MMORPG」——建议 Unity 6.3 LTS

1. **大世界流式加载**：Addressables 成熟可用；而 Godot 在大规模 3D 流式与烘焙光照上是**已知短板**，恰是本项目的主战场。
2. **C# 适合大型代码库**：静态类型、重构能力、IDE 支持均优于 GDScript——对 90~110 个任务量级的工程是实质收益。
3. **性能剖析与优化工具链成熟**（Profiler、Memory Profiler、Frame Debugger）。
4. **AI 工具链**：Unity AI（编辑器内 + 工程上下文）+ Unity MCP（外部 Agent 可读写场景、程序化跑单测，可配置为 WorkBuddy MCP connector）+ 训练数据密度最高。
5. **生态与先例**：Asset Store MMO 套件、大量已上线 MMO 案例；Godot 在此规模上几乎没有已上线先例。
6. **C++ 复用不受损**：Unity 支持 Windows 原生插件（C++ DLL），协议层（FlatBuffers 解码、AOI Delta 应用、插值预测）同样可下沉 C++，与服务端共享 schema——此优势并非 Godot 独占。

### 4.3 建议技术栈（Unity 路线）

```
Unity 6.3 LTS（URP，不用 HDRP）
  ├── C#                 —— 游戏逻辑、UI、状态机、场景管理
  ├── C++ Native Plugin  —— 网络/协议热路径（FlatBuffers 解码、AOI Delta 应用、插值预测）
  ├── Addressables       —— 分区资源流式加载（对应规范 §21 / §22）
  └── Unity MCP          —— 接入 WorkBuddy / Cursor 作为外部 Agent 通道
```

### 4.4 选定前必须先做的两件事

1. **企业代理激活冒烟测试（最高优先级，成本极低）**
   在你的企业代理环境下安装 Unity Hub 并完成 Personal 许可证激活。
   **若激活失败 → Unity 直接出局，无需再评估其余维度。**
2. **上调 Low 档目标并走 RFC 修订规范 §21 / §35**
   建议改为：Low = 4 核 / **8GB RAM / 2GB VRAM / DX11**；
   或保留 4GB，但明确写「仅限内容预算极度收敛的分区场景，以实测容量报告为准」。

### 4.5 成本提示

| 项 | 费用 |
|---|---|
| Unity Personal | 免费（营收/融资 < $100k） |
| Unity AI（Personal） | 试用后 **$10/月**（1000 credits） |
| Unity Pro（含 Unity AI） | **$185/月/席** |
| Unity MCP | MIT 免费（外部订阅如 Cursor Pro 约 $20/月） |

### 4.6 最终决策：回切 Godot 4.7.1（2026-08-29）

> **决策变更**：本节 §4.2–§4.5 曾基于「目标是大型 MMORPG」推荐 Unity 6.3 LTS，且已按该路线批准（§7.1 原文）。**现正式回切至 Godot 4.7.1**，依据如下：

1. **代理致命条件已触发**：§7.3 的冒烟测试预检显示，当前企业代理（`127.0.0.1:64425`）对 Unity 安装/激活路径（unity.com、license.unity3d.com、download.unity3d.com 等）整体拦截（502/000），Personal 许可证无法激活。Unity 路线的核心前置失败。
2. **用户主观选择**：用户明确"我觉得更适合我"——Godot 的全文本工程（.tscn/.tres/.gd）、AI 可直接生成/校验 GDScript、编辑器秒开、免激活、完全离线（绕过代理墙）、MIT 免费，对每天 1–2 小时、企业代理环境的独立开发者更顺手。
3. **权衡后接受 Godot 的短板**：大规模 3D 流式与烘焙光照是 Godot 已知短板，但本项目本就是 Scene/Instance 分区 + AOI + Chunk Streaming，不做无缝大地图，规避了该短板的主战场；且 Godot 4.7 的 Forward+ 渲染器（Vulkan/D3D12/Metal）足以支撑分区 3D 表现。

**C++ 复用不受影响**：Godot 通过 **GDExtension(C++)** 下沉协议/热路径（FlatBuffers 解码、AOI Delta 应用、插值预测），与服务端共享 schema——这与 Unity 的 C++ Native Plugin 等价，原 §4.2-6 的论点现在反而成为 Godot 路线的同等优势。

> **Unity 路线不删除，保留为 §6.2 可选回切**：若未来企业代理解除（加白名单或网络环境变化），可重新评估 Unity；届时 §6.1/§6.2 角色互换即可，无需重开 RFC。

---

## 5. 风险与缓解

### 5.1 选 Unity 6 的风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **企业代理下 Hub 安装 / 许可证激活失败** | **致命——Unity 出局** | 开工前先做冒烟测试（§4.4-1）；失败即转 Godot |
| Low 档 4GB / 1GB VRAM 难达成 | 与规范 §21/§35 冲突 | 上调最低配置并走 RFC；或靠内容预算硬控，实测后写入容量报告 |
| 营收超 $100k 需转 Pro（$185/月/席） | 成本上升 | 上线前不会触发；届时再评估 |
| 编辑器导入 / 构建耗时长，挤占每天 1–2h | 迭代变慢 | 批量操作走 Unity MCP；重构建走 `-batchmode` 后台跑 |
| Unity AI 产出偏「教程同质化」 | 需人工改造 | 仅用于脚手架与原型；核心系统自研 |
| 无头 CI 门禁较绕（MCP 依赖编辑器实例） | 自动化验收成本上升 | `verify/*.sh` 封装「启动编辑器 → 跑 EditMode/PlayMode 测试 → 退出」；纯 C# 逻辑层用 `dotnet test`，不依赖编辑器 |

### 5.2 选 Godot 4.7 的风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| **大规模 3D 流式加载与烘焙光照是已知短板** | 大世界卡顿、光照质量受限 | 架构本就是 Scene/Instance 分区 + AOI + Chunk Streaming，规避无缝大地图 |
| 3D 渲染上限低于 Unity | 画质天花板 | 规范 §20/§35 明确「优先低资源而非高画质」 |
| GDScript 性能弱于 C# | 高频逻辑掉帧 | 高频路径下沉 **GDExtension C++** |
| 大型 MMO 几乎无已上线先例 | 未知坑多 | 更保守的原型验证；垂直切片先行 |
| 生态与现成资产少于 Unity | 自制工作量上升 | 美术走外购 / AI 生成 |
| 主机导出需第三方移植商 | 无影响 | 本项目仅目标 PC |

---

## 6. 变更影响面（已批准：Godot 4.7.1 路线，按 §6.1 执行）

> **已批准**：路线锁定 **Godot 4.7.1**。原 Unity 路线降级为 §6.2 可选回切（若代理解除）。

> **关于「AI 生成模型」的能力边界（2026-08-29 记录，避免后续执行偏差）**：
> Godot（及任何引擎）的 AI 协作是**工程操作层**——外部 AI（WorkBuddy）直接生成/校验 **GDScript 代码、.tscn 场景、资源导入配置**，这是 Godot 全文本工程的天生优势。**但 AI 不生成 3D 几何模型本身。**
> 用户此前提到的"生成模型"，应拆为两层：
> 1. **几何/贴图生产**（mesh + texture）：需程序化几何代码、专用 AI 建模工具（Meshy / Tripo / Luma）或美术/DCC（Blender）生产；
> 2. **工程集成**（把模型导入、自动摆放、挂脚本、连入场景）：**由 GDScript 代码 + 场景树承担**，AI 可直接写这部分代码，用户的方向判断正确。
> 后续客户端任务中，资产生产任务与工程集成任务需分开编排。

### 6.1 Godot 路线（当前执行方案）

| 任务 | 变更 |
|---|---|
| **TASK-034 Client Core** | 改为「**Godot 4.7.1 工程骨架** + 应用生命周期 / 场景树管理 / 目录分层（autoload + 子场景）」。去掉自研主循环与自研窗口管理部分 |
| **TASK-035 Renderer** | **重写**：由「自研 D3D11 渲染器」改为「**Godot 渲染管线配置 + 兼容性渲染器（DX11 / OpenGL 3.3）为 Low 档 + Forward+（Vulkan/D3D12/Metal）为 High 档 + LOD / 合批 / 距离剔除**」，**删除「禁止引入重型第三方引擎」约束** |
| **TASK-036 Resource / Low Spec** | 改为「**Godot 资源管线 + 导入配置（.import） + 三档画质预算（贴图/面数/DrawCall/音频常驻）+ Chunk Streaming 与 LOD 策略 + 容量实测报告**」 |
| **TASK-041 跨进程集成** | 回归用例增加「**Godot 客户端 `--headless --export-release` 构建 + 连接真实服务端冒烟**」门禁 |
| **新增客户端任务（15–20 个）** | 网络层 / 登录与角色选择 / 移动与预测回滚 / 战斗表现 / UI 框架 / 背包 / 任务 / 地图与寻路 / 音频 / 设置与画质切换 / 崩溃上报 / 打包流水线 |
| **_common.sh** | 增加 `require_godot`（探测 `godot` 可执行文件与版本，兼容性渲染器需 Vulkan 1.0 或 DX11）+ `godot_build_win64()` 与 `godot_run_tests()` 封装函数 |
| **服务端侧（不变）** | FlatBuffers schema、AOI Delta、协议层**完全不变**，仅客户端侧以 GDExtension(C++) 或 GDScript 实现解码；不触碰任何服务端任务 |

**目录分层与依赖单向要求**——保证客户端内部不相互干扰，与服务端 §27 同一思想（Godot 无 asmdef，用目录约定 + 静态扫描 enforce）：

```
client/runtime/      —— 核心运行时（autoload，不依赖任何上层）
client/network/      —— 传输与协议（依赖 runtime）
client/gameplay/     —— 战斗/技能/任务表现（依赖 runtime + network）
client/ui/           —— UI 层（依赖 runtime + gameplay，反向依赖禁止）
client/extensions/   —— GDExtension(C++) 构建产物与绑定（依赖 runtime）
```

依赖方向即目录引用方向，验收脚本用静态扫描强制单向（对应 `scan_forbidden` 的 GDScript 变体：禁止 `client/ui/` 反向 import 上层目录）。

### 6.2 Unity 路线（可选回切，当前非执行方案）

> 若未来企业代理解除（白名单或网络环境变化），可重新评估 Unity 6.3 LTS。本节保留为回切依据，角色届时与 §6.1 互换，无需重开 RFC。

| 任务 | 变更（回切时启用） |
|---|---|
| TASK-034 Client Core | 改为「Unity 6.3 LTS（URP）工程骨架 + 程序集划分（asmdef）」 |
| TASK-035 Renderer | 改为「URP 渲染管线 + 三档 Quality Level + LOD / SRP Batcher / GPU Instancing」 |
| TASK-036 Resource / Low Spec | 改为「Addressables 资源管线 + 分区 Bundle + 三档画质预算」 |
| TASK-041 跨进程集成 | 增加「Unity `-batchmode -buildWindows64Player` + EditMode/PlayMode 测试 + 连真实服务端」门禁 |
| _common.sh | 增加 `require_unity` / `require_dotnet` + `unity_build_win64()` / `unity_run_tests()` |

---

## 7. 决策状态（2026-08-29 批准 → 同日回切 Godot）

### 7.1 已决（已拍板）

1. ✅ 采用 **Godot 4.7.1** 替代 TASK-035 的自研 D3D11 渲染器；「禁止引入重型第三方引擎」约束**已撤销**。
2. ✅ 渲染管线用 **兼容性渲染器（DX11 / OpenGL 3.3）为 Low 档 + Forward+（Vulkan/D3D12/Metal）为 High 档** 双档。
3. ✅ **AI 协作方式**：直接由 WorkBuddy 生成/校验 **GDScript 代码、.tscn 场景、资源导入配置**（Godot 全文本工程天生适配）；不引入 Unity MCP（路线已切）。
4. ✅ 工具链方向：**GDScript 游戏逻辑 + GDExtension(C++)（协议/热路径下沉）+ Godot 资源管线 + 全文本 AI 协作**。
5. ✅ **回切机制**：Unity 路线保留为 §6.2 可选回切，若企业代理解除可重新评估，无需重开 RFC。

### 7.2 待办（仍需执行）

1. ⏸️ **客户端任务（TASK-034/035/036 + 新增 15–20 个）延后**：用户决策（2026-08-29）— 暂不安装/实现任何客户端引擎，先搭服务端和游戏核心。**待引擎确定 / 需要表现层时**，再按 §6.1 用生成器重写并新增，落地 Godot 目录分层与依赖单向约束、双档画质预算、headless 构建门禁。
2. ⏳ **落地引擎中立纪律**：参照 `docs/engine-agnostic-architecture.md`，在 gameplay 代码评审与 CI 中执行 §3 红线（规则只在 Lua/C++、协议用 FlatBuffers、配置中立格式、美术源用 glTF/FBX）；并冻结 FlatBuffers 协议 schema（切换引擎的契约基础）。
3. ⏳ **RFC 子任务：修订项目总规范 §21 / §35**：Low 档目标明确（4 核 / 4GB RAM / 1GB VRAM 为激进值，需内容预算硬控 + 实测容量报告；或上调至 8GB/2GB VRAM 并走 RFC）。
4. ⏳ **资产生产 vs 工程集成任务拆分**：依据 §6 能力边界说明，把"程序化几何 / AI 建模工具 / 美术或免费包"与"客户端工程集成"分别编排到客户端任务中（延后，同 §7.2-1）。
5. ⏳ **Godot 编辑器获取（绕过代理，时机待定）**：从 `godotengine.org` 下载 4.7.1 标准版 ZIP（约 72MB）—— 经当前代理预检该域名可能亦被拦，需用手机热点/家庭网络等一次性获取，之后完全离线可用、无需激活。

### 7.3 代理冒烟测试结论（2026-08-29，已作为回切依据）

- 当前企业代理（`127.0.0.1:64425`）对常规流量正常（github / example.com 200），但 **Unity 安装/激活路径整体被拦**：`unity.com`(502)、`hub.unity.com`(000)、`accounts.unity.com`(000)、`license.unity3d.com`(502)、`download.unity3d.com`(000)、`public-cdn.unity3d.com`(000)。
- 可达：`packages.unity.com`(200)、`developer.cloud.unity3d.com`(302)、`api.unity.com`(403)。
- **判定**：Unity 路线致命前置触发；用户选择 Godot 路线，规避该问题（Godot 单 ZIP 离线、免激活，但仍需一次性联网下载编辑器 ZIP）。

---

## 8. 参考

- Godot 官方系统需求（含导出产物最低配置）：https://docs.godotengine.org/zh_CN/stable/about/system_requirements.html
- Godot 渲染架构（兼容性渲染器限制）：https://docs.godotengine.org/en/latest/engine_details/architecture/internal_rendering_architecture.html
- Unity 6.3 系统需求：https://unity.com/en/unity/system-requirements
- Unity AI（原 Muse）：https://unity.com/products/unity-ai
- Unity MCP（开源，MIT）：https://github.com/CoderGamester/mcp-unity （社区实现，接入前请自行审计）
- 项目总规范 V1.0 §20 / §21 / §35
- 缺口路线图：`docs/game-completion-roadmap.md`
