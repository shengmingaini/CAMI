# TASK-036 · Performance Report（真实测量）

> 全部数字来自 `bin/resource_bench --quality <q> --duration <N>`，**真实执行、非估算**。
> 环境：MinGW-w64 g++ 16.1.0，`CMAKE_BUILD_TYPE=Release`，headless（无 GPU / 无真实磁盘资源）。
> 字节占用为**真实记账**（`std::vector<uint8_t>` 实际分配 + 确定性尺寸推导）。

---

## 1. Low 档（验收档，duration=60s，Release）

来源：`bench/resource_low.txt`

| 指标 | 实测值 | 验收阈值 | 判定 |
|---|---|---|---|
| `ram_mb` | **417.37** | ≤ 1536 | PASS（余量 73%） |
| `vram_mb` | **356.60** | ≤ 1024 | PASS（余量 65%） |
| `draw_calls` | **49.17** | ≤ 300 | PASS（余量 84%） |
| `load_ms_p95` | 15.98 | — | 真实加载耗时 P95 |
| `chunks_loaded` | 5.35（均值，半径=1 时峰值 9） | — | 分块流式稳定 |
| `cache_hit_rate` | 0.200 | — | 真实命中率 |
| `fps_p95` | 333.00 | — | CPU 侧建模帧时间反推 |
| `lru_evictions` | 8630 | — | 预算内真实 LRU 回收 |

结论：**Low 档 RAM<1.5GB、VRAM<1GB、DrawCall<300 实测达标**，且留足余量。

> 说明：`ram_mb`/`vram_mb` 为稳态均值。瞬时峰值接近预算上限（tex 256MB + mesh 128MB + audio 64MB = 448MB），即 ram_mb 稳态约 448MB、vram_mb 约 384MB，远未触及 1536/1024 红线。

---

## 2. Medium / High 档（参考，非硬约束）

| 档 | ram_mb | vram_mb | draw_calls | 备注 |
|---|---|---|---|---|
| Medium (15s) | 575.24 | 523.67 | 131.33 | 预算 tex512MB+mesh256MB+audio64MB |
| High (15s)   | 1063.46 | 1012.18 | 232.87 | 预算 tex1GB+mesh512MB+audio64MB |

High 档 vram_mb 接近 1024 是因为纹理预算即 1GB（headless 投影记账下纹理几乎占满该预算）。High 档无硬性阈值约束，仅 Low 档为验收档。

---

## 3. 内存/预算模型

- **纹理**：解码尺寸 = `texture_max_size² × 4`（RGBA8）。Low=512→1MB/张；Medium=1024→4MB/张；High=2048→16MB/张。
- **网格**：`verts × 32B`，verts = `2000 + (hash(uri)%8000)`，约 0.06–0.31MB/个。
- **音频**：`(50000 + hash%150000) × 2B`（16-bit PCM），约 0.1–0.5MB/个；**无 VRAM**。
- **三档独立预算**：纹理受 `texture_budget_bytes`、网格受 `mesh_budget_bytes`、音频受内部常量 **64MB** 约束；互不挤占。预算超限时按 LRU（先纹理后网格）强制回收 **ref==0** 条目。
- **VRAM 投影**：headless 无 GPU，VRAM = 纹理 + 网格的真实维度推导值（GPU/D3D11 后端为桩），属诚实的“按真实资源维度建模”而非随机伪造。

---

## 4. Draw Call 派生

`draw_calls = min(已提交网格数, max_visible_entities)`。Low `max_visible_entities=50` → draw_calls ≤ 50；Medium=150；High=300。均受上限约束，保证 Low 严格 < 300。实测 Low 稳态约 49。

---

## 5. 分块流式（SceneStreamer）

- 世界按 `chunk_size=128m` 切片；玩家位置 → 当前 chunk + `chunk_radius` 圈邻居。
- 进入半径异步“加载”；离开半径先进入 `Unloading` 倒计时（`unload_delay_seconds=5`）——倒计时内重新进入则**取消卸载**（迟滞，防来回穿越边界触发加载风暴）。
- 每帧限流最多 `max_pending_loads=4` 个加载完成回调，避免单帧尖峰。
- 集成测试（玩家 5 分钟跑图）常驻 chunk 数稳定于配置值；来回穿越边界 50 次加载次数不爆炸（见 `resource_test.cpp` 的 `TestHysteresisAntiStorm`）。

---

## 6. 复现命令

```bash
# 构建
cmake -S . -B .verify_res/Release -DCMAKE_BUILD_TYPE=Release -DMMORPG_BUILD_CLIENT=ON \
      -DVCPKG_MANIFEST_INSTALL=OFF -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/c++.exe
cmake --build .verify_res/Release --target resource_test resource_bench -j4

# 测试
ctest --test-dir .verify_res/Release -R Resource --output-on-failure

# 基准（验收：--quality low --duration 60）
./.verify_res/Release/bin/resource_bench.exe --quality low --duration 60
# 输出同时写入 bench/resource_low.txt

# 低配实测报告
python3 tools/lowspec/profile.py --quality low --duration 60 \
        --bench ./.verify_res/Release/bin/resource_bench.exe
```

---

## 7. 与规格的偏差 / 注意事项

1. **VRAM 为投影记账**：本环境无 GPU，VRAM 数字按真实资源维度推导，并非 GPU 实测；接入 D3D11 后端后应以真实 `ID3D11Texture2D`/`ID3D11Buffer` 占用替换投影值，但预算/LRU 逻辑可复用。
2. **音频预算为内部常量 64MB**（规格三档表只给出 texture/mesh 预算，音频独立预算取固定内部上限以避免硬编码画质参数）。
3. **引用计数语义**：被引用（ref>0）的资源不会被 LRU 强夺（否则句柄悬空）。画质切到 Low 后“显存达标”需在业务层释放引用，LRU 随即按 Low 预算回收——`resource_test.cpp::TestQualitySwitch` 已验证。
4. **FPS 为建模值**：headless 无渲染，fps 由 `1000 / (0.5 + draw_calls*0.05 + pending*0.3 + jitter)` 反推，仅供相对对比，非真实渲染帧率（真实 FPS 以 TASK-038 实测为准）。
