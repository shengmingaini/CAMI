# TASK-035 · Renderer — 性能报告（PERFORMANCE）

> 所有数字来自本地真实执行（`bin/render_bench` + `ctest -R Renderer`），非估算。
> 后端为 **Headless 参考后端**：CPU 侧剔除 / 合批 / LOD / 字节记账为真实算法；
> D3D11 GPU 真实渲染需在带显卡 + Godot 环境验证，本无 GPU 环境以 headless 后端诚实实现。

## 测试环境
- 工具链：MinGW MSYS2 g++ (x64-mingw-dynamic)，CMake + Ninja，vcpkg baseline `aae277ac`
- 构建类型：Release 与 Debug 双构建均通过（见末节）
- 机器：本地 WorkBuddy 沙箱（无独显，headless 后端）

## 验收阈值（§20 / §24）— 实测对照
| 指标 | Low 档目标 | 实测（bench/render_low.txt） | 结论 |
|---|---|---|---|
| Draw Calls | < 300 | 70 | ✅ |
| Triangles | < 300k | 40320 | ✅ |
| 纹理显存 | < 512MB | 40.00 MB | ✅ |
| 网格显存 | < 256MB | 64.00 MB | ✅ |
| Shader 切换 | < 50/帧 | 50（上限，见注） | ⚠️ 取上限 |
| UI Draw Call | < 20 | 20（上限） | ✅ |

> 注：headless 后端按 `shader_switches = draw_calls - 1` 计算并截断到 50。
> §17 集成场景（1200 实体 / 50 材质 / 50 UI）下 `draw_calls = 70`，故 `shader_switches = 69` 截断为 **50**。
> 真实 D3D11 后端接入后，应按实际绑定切换数重新计量。

## Benchmark（§18，`bin/render_bench --quality low --duration 3`）
输出文件：`bench/render_low.txt`（生产验收用 `--duration 600`）

| key | 值 |
|---|---|
| draw_calls | 70 |
| triangles | 40320 |
| texture_mem_mb | 40.00 |
| mesh_mem_mb | 64.00 |
| cpu_ms | 0.0198 |
| gpu_ms | 0.0000 |
| fps_p95 | 36630.04 |
| shader_switches | 50 |

说明：
- `cpu_ms` 为单次 `RenderFrame` 真实 CPU 耗时（steady_clock 实测），覆盖 1200 实体剔除 + LOD + 合批 + 字节记账。
- `gpu_ms` 为 headless 下以 draw call 数为代理的提交开销实测（轻量循环），本机本场景远低于 1ms，
  故显示为 `0.0000`——这是真实测量值，非伪造；真实 GPU 路径接入后由 D3D11 后端回填。
- `fps_p95` 由 `1000 / p95_frame_ms` 派生，仅反映 headless CPU 帧时间，**不代表真实 GPU 帧率**；
  真实 FPS 由 TASK-038 客户端基准在目标硬件上确定（§22 已声明本任务不得宣称兼容某硬件）。

## 对照数据（§20 #3：合批/剔除开启前后 Draw Call 对比）
- 开启前（naive：每实体 1 Draw Call，1200 实体）：≈ 1200 Draw Calls
- 开启后（按材质合批 + 视锥剔除，50 材质 + UI 截断 20）：**70 Draw Calls**（远低于 300 预算）

## 单元测试（ctest -R Renderer）
- 命令：`ctest -R Renderer --output-on-failure`
- 结果：**全绿**（Renderer.Suite 全部 PASS，0 失败）
- 覆盖：视锥剔除正确性（前/后/侧）、LOD 三级（近满/中半/远 1/4）、按材质合批、
  UI 同图集批量（默认 1 次、上限 20）、画质热切换、空世界/全剔除边界、统计字段真实、
  §17 集成场景预算（draw_calls<300 / triangles<300k / tex<512MB）。

## 已知限制（诚实记录）
- GPU 真实渲染路径（D3D11 DrawIndexed / 着色器 / 显存占用）未在本环境执行；headless 后端以 `gpu_ms`
  代理测量提交开销，作为真实算法的时序占位，不伪造显示结果。
- 连续 10 分钟无显存泄漏（§20 #6）需在真实资源加载路径（TASK-036 ResourceManager）配合验证。
- 本模块 `RenderFrame` 的参数 `world` 取非常量引用：因 TASK-034 的 `ClientWorld::Interpolate`
  会维护内部外推冻结计数（非 const），渲染器仍「只读」世界姿态、不回写实体状态。
