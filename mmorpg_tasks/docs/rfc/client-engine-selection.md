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
   > ⚠️ **本条已被 §9（2026-09-10）部分修正**：客户端当前阶段为 **2D**，仅启用 **Compatibility 渲染器单档**；
   > Forward+ 高档**推迟到引入 3D 表现时**再启用。原始双档设计保留，作为 3D 阶段的承接基线。
3. ✅ **AI 协作方式**：直接由 WorkBuddy 生成/校验 **GDScript 代码、.tscn 场景、资源导入配置**（Godot 全文本工程天生适配）；不引入 Unity MCP（路线已切）。
4. ✅ 工具链方向：**GDScript 游戏逻辑 + GDExtension(C++)（协议/热路径下沉）+ Godot 资源管线 + 全文本 AI 协作**。
5. ✅ **回切机制**：Unity 路线保留为 §6.2 可选回切，若企业代理解除可重新评估，无需重开 RFC。
6. ✅ **表现层形态：2D 优先**（2026-09-10 用户确认）——**现阶段不做 3D 几何模型**；客户端按 2D 规格执行，
   3D 表现作为后续演进保留（见 §9）。

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

---

## 9. 追加决策：2.5D（isometric / billboard faux-3D）（2026-09-14 由 2D 优先升级）

> **本节是对 §4.6 / §6.1 / §7.1 的追加修正，不改变「引擎＝Godot 4.7.1」这一结论。**
> 用户决策原文：*「用 Godot 4.7.1 吧，现阶段不太可能做 3D 的模型。」*
> 即：引擎选择维持不变，表现层形态收敛为 2D。
>
> **2026-09-14 升级**：用户决策将表现层形态由纯 2D 升级为 **2.5D**（3D 世界坐标 + 透视 / 等距相机 + 2D 精灵美术，不生产 3D 几何模型）。完整规格见 **`docs/client-spec-2.5d.md`**（本 RFC §9 仅保留决策要点，规格书为唯一权威依据）。
> 实现参数已定：相机固定俯角 **45°**、精灵 **8 向**、Godot 用 **4.7.2**（已解压核验 `4.7.2.stable.official.ed1daf0bf`）；其余画质参数采用规格书推荐值。

### 9.1 依据（三条，均为本项目既有限制的直接推论）

1. **3D 几何/贴图生产是整链最大不可自动化项** —— §6 已明确「AI 不生成 3D 几何模型本身」，
   它需要程序化几何代码、专用建模工具（Meshy/Tripo/Luma）或 DCC（Blender）生产。
   对每天 1–2 小时的独立开发者，这是**唯一加班也解决不了**的瓶颈。
2. **2D 资产的可获得性与 3D 差一个数量级** —— 免费/付费 2D 图集、TileSet、精灵包成熟且便宜；
   3D 角色模型 + 骨骼 + 动画集外购成本高、风格统一难。这直接消解了「做不了 3D 模型」的约束。
3. **2D 完全规避 Godot 已登记的最大短板** —— §5.2 / §7.1-3 记录的 Godot 风险是
   「大规模 3D 流式加载与烘焙光照是已知短板」。**2D 阶段该风险归零**，
   即 2D 决策让本项目最脆弱的引擎风险消失了（这是选 Godot 的额外收益）。

### 9.2 对已决项的修正（逐条对应）

| 原条目 | 原文 | §9 修正后 |
|---|---|---|
| §7.1-2 渲染档位 | 兼容渲染器（Low）+ Forward+（High）**双档** | **仅 Compatibility 单档**（OpenGL 3.3 / DX11）。Forward+ 推迟至 3D 阶段 |
| §6.1 TASK-034 | Godot 工程骨架 + 生命周期/场景树/目录分层 | **不变**（2D/3D 通用）；去掉自研主循环与窗口管理的结论同样不变 |
| §6.1 TASK-035 | Godot 渲染管线 + 双档 + LOD / 合批 / 距离剔除 | 改为 **2.5D 渲染管线**：`Camera3D` 透视固定俯角 + `Sprite3D`/`AnimatedSprite3D` billboard + 地面程序化网格 + 深度遮挡 + LOD / 远精灵降级 + 图集合批；渲染器仅 Compatibility 单档（见 `docs/client-spec-2.5d.md` §2–§3） |
| §6.1 TASK-036 | 资源管线 + 三档预算（贴图/面数/DrawCall/音频）+ 3D Chunk Streaming | 改为 **2.5D 资源管线**：`AtlasTexture`/`.import` + 程序化网格缓存；**分块流式改 3D 世界分块（九宫格/半径加载）**；三档预算增加三角面 / 精灵图集分辨率 / 地面网格密度（见 `docs/client-spec-2.5d.md` §4–§8） |
| §7.2-2 美术源格式 | 美术源用 glTF / FBX | 改为 **PNG（带 alpha）/ SVG（UI 图标）/ 图集 `.tres`**；glTF/FBX 仅 3D 阶段启用 |
| §6.1 新增客户端任务数 | 15–20 个 | **12–16 个**（见 §9.4） |

### 9.3 性能预算口径改写（2.5D）

原 3D 口径（DrawCall / 三角面 / LOD / 阴影）与纯 2D 口径（图集切换 / Canvas 重绘）在 2.5D 下取并集（完整口径见 `docs/client-spec-2.5d.md` §4），要点：

- **DrawCall** 与 **图集切换次数**（2D 的等效瓶颈，靠同图集合批 + `TileMapLayer` 分层控制）
- **同屏精灵/图元数** 上限
- **Canvas 重绘面积 / 每帧填充率**（大尺寸半透明 UI 与特效是 2D 的主要开销）
- **纹理显存**（图集分辨率之和）与 **音频常驻/流式** 划分
- **Low 档目标**（4 核 / 4GB RAM / 1GB VRAM）：2D 下从 §7.2-3 所述的「激进值」变为**宽松值**——
  该待办项的压力显著下降，但**仍需实测容量报告**（TASK-036 交付）。

### 9.4 任务预算修正

省掉（3D 专属）：蒙皮/骨骼动画管线、光照与烘焙、3D 地形与 NavMesh 客户端、LOD 制作、材质系统复杂度。
保持不变（工作量主体）：网络与同步（协议对接/预测/插值）、输入与相机、UI 全套（登录/选角/HUD/背包/技能栏/地图/聊天/设置）、音频、场景加载、打包流水线。

→ **新增客户端任务：14–17 个**（原 15–20；纯 2D 为 12–16，2.5D 因相机 / 精灵 / 地面网格专项略增）。省下的大头是 3D 角色 / 光照资产管线，不是代码量。

### 9.5 不变项（重要，防止过度解读）

- **服务端全部不变**：本决策只影响表现层，TASK-000~041 的服务端部分与协议层零改动。
- **协议契约不变**：FlatBuffers schema、AOI Delta、快照格式完全不变。
- **GDExtension(C++) 下沉不变**：协议解码 / 热路径仍走 GDExtension，C++ 复用红利保留。
- **目录分层不变**：`client/{runtime,network,gameplay,ui,extensions}` 与依赖单向约束不变。
- **`require_godot` 门禁方向不变**，但渲染器探测条件放宽（2D 只需 OpenGL 3.3 / DX11，不要求 Vulkan）。

### 9.6 2D → 3D 演进的唯一前提（必须从第一天遵守）

> **逻辑层不得解算表现。** 位置、朝向、状态一律由服务端数据与配置驱动，
> 客户端表现层只做「把数据翻译成画面」。

只要守住这条，未来引入 3D 时**只需替换表现层**（精灵 → 模型、TileMap → 地形网格），
逻辑与网络层零改动；违反它则 2D 的临时决策会变成永久技术债。
此条与 `docs/engine-agnostic-architecture.md` §3 的引擎中立纪律同源，应在客户端代码评审中一并执行。

### 9.7 开放项（客户端阶段开工前必须决策）

1. **2.5D 画风 / 视角**：俯视四方向（top-down）vs 等距（isometric）vs 固定俯角透视——
   影响地面网格密度、精灵方向数（4 向 / 8 向）、相机跟随方式（见 `docs/client-spec-2.5d.md` §13 待确认）。
2. **Godot 编辑器获取**：同 §7.2-5（一次性联网，约 72MB ZIP，之后离线可用）。
3. **TASK-034/035/036 重写时机**：见 §9.8。
4. ~~**是否补充「2.5D 过渡档」**：等距视角 + 2D 精灵可近似 3D 观感，是 2D 到 3D 之间的低成本中间态~~ —— **已采纳**：2.5D 现为当前正式形态（见 `docs/client-spec-2.5d.md`），本条关闭。

### 9.8 执行时机（与 §7.2-1 一致：客户端延后）

客户端阶段的执行触发条件不变——**先完成服务端与游戏核心（TASK-030~033/037/039/040/041）**，
待需要表现层时，再按 §6.1 + §9 用生成器重写 TASK-034/035/036 并新增 12–16 个客户端任务。

> **注意**：TASK-038（Bot + Load + Chaos 判定点）的依赖中含 TASK-034/036，
> 但其交付物是**协议层 Bot**（复用 TASK-005，不依赖任何引擎）。
> 该依赖是否必要**待复核**——若 Bot 可独立于游戏客户端实现，
> 则可解除 038 对 034/036 的依赖，让 50k CCU 判定点不再被客户端阶段阻塞。
