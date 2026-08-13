# CI 资源优化：OOM 根因诊断与治理（2026-08-12）

> 背景：Week3/4（网关模块 + 数据层生产接入）期间 `build-modules-on` 重型 job 频繁卡顿、
> 爆内存（ubuntu-latest runner 仅 16GB）。本文档记录根因诊断、优化措施与收益预估。

## 一、根因诊断（按贡献排序）

| # | 根因 | 说明 | 后果 |
|---|------|------|------|
| 1 | **链接阶段无并发上限** | CI 用默认 Makefiles 生成器，`cmake --build build-mod -j2` 允许 2 个 target 同时链接；`cami_data_service_main`（链 gRPC+Protobuf）、`cami_security_test`（链 OpenSSL）、`cami_redis_test`（链 redis++）任一链接峰值 ~2-3GB，并行链接直接冲击 16GB | **OOM 主因** |
| 2 | **无编译缓存** | 项目自身代码（含大型 protobuf 生成代码 `data_service.pb.cc`/`.grpc.pb.cc`）每次 CI 全量重编，build 目录也不缓存 | 重跑慢 + 内存峰值反复出现 |
| 3 | **vcpkg 无版本锁定** | `git clone --depth 1` 每次拉最新 vcpkg，依赖版本每日漂移；bincache key 只含 `vcpkg.json` hash，上游 ABI 变化后缓存失效，重触发 gRPC 等全量源码构建（~1.5h） | bincache 命中率不稳定 |
| 4 | **无 workflow 并发控制** | 同分支连续 push 启动多个重型 run，抢 runner 排队 + GitHub 资源池堆叠 | "CI 卡住"体感来源 |
| 5 | **链接器为 GNU ld** | 比 lld 慢 2-5x 且内存峰值更高 | 放大根因 1 |

## 二、优化措施（已落地）

| 改动文件 | 措施 | 效果 |
|----------|------|------|
| `.github/workflows/ci.yml` | ① 顶层 `concurrency`：同 ref 连续 push 自动取消旧 run | 杜绝多 run 抢资源/排队 |
| | ② 两个 C++ job 改用 **Ninja 生成器** | 支持 job pools（Makefiles 无此能力） |
| | ③ apt 装 **ccache + lld**，configure 传 `-fuse-ld=lld` | 编译缓存 + 低内存快速链接 |
| | ④ ccache 目录 `actions/cache` 持久化（`CCACHE_DIR` 固定到 workspace，`CCACHE_MAXSIZE` 限 1G/2G） | 代码未变时重跑编译≈0 |
| | ⑤ vcpkg root 目录缓存（`Install vcpkg` 步骤 `cache-hit` 时跳过） | 省每次 clone+bootstrap |
| `CMakeLists.txt` | ⑥ **Ninja job pools 默认值**：`compile=2;link=1`（链接串行，峰值=单 target） | OOM 硬闸，本地 Ninja 构建同样受保护 |
| | ⑦ **ccache 自动探测**：`find_program(ccache)` 命中且未显式指定 launcher 时自动启用 | 本地/CI 统一受益，无 ccache 环境零影响 |
| `vcpkg.json` | ⑧ 锁定 `builtin-baseline`（vcpkg 2026-08-12 最新 commit `aae277ac`） | 依赖版本固定，bincache key 稳定，命中率显著提升 |

## 三、收益预估（16GB runner）

| 场景 | 优化前 | 优化后 |
|------|--------|--------|
| 全量首次（bincache miss） | vcpkg 源码构建 ~1.5h + 项目全量编译 | 基本不变（vcpkg 构建是下限） |
| **重跑（代码小改）** | 项目全量重编 ~10-20min | ccache 命中，编译 <1min |
| **链接阶段内存峰值** | 2 个 gRPC 级 target 并行链接 ~4-6GB+（OOM 风险） | 单 target 串行 ~2-3GB（安全） |
| bincache 命中率 | 受 vcpkg 每日漂移影响，随机失效 | baseline 锁定后稳定命中 |
| 连续 push | 多 run 并行排队 | 旧 run 自动取消，只跑最新 |

## 四、验证

- 本地 Ninja + `CAMI_BUILD_MODULES=OFF` 全链路：configure 日志确认 `Ninja job pools: compile<=2, link=1 (OOM guardrail)` 生效；构建 + ctest 全绿（见当日记录）。
- 无 ccache 环境（本机 MSYS2 未装 ccache）：`find_program` 未命中即跳过 launcher，构建路径零影响。
- MODULES=ON 路径的 Ninja 参数与 `-fuse-ld=lld` 需下一次 CI push 验证（本地无 vcpkg 全量工具链）。

## 五、遗留/建议

1. **`-fuse-ld=lld` 若在 MODULES=ON 下与 gRPC 静态库链接冲突**（理论风险低），回退方法：删除 CI 命令行中该 flag 即可，其余优化不受影响。
2. 若 bincache 首次恢复后仍偶发 vcpkg 源码构建（如新增 port），可把 `VCPKG_MAX_CONCURRENCY` 保持 1（已是最稳配置）。
3. 后续可考虑：把 `build-modules-on` 拆分为 deps-build / project-build 两个 job（cache 跨 job 复用，失败隔离），但会引入重复构建开销，当前单 job + 缓存方案已足够，不建议立即拆分。
