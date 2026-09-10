# -*- coding: utf-8 -*-
"""
MMORPG 任务包生成器（方案 A · 原地补齐）

数据源：tools/gen/data_a.py ~ data_d.py（39 个 TASK 的唯一真相源）
产出：
  - tasks/TASK-000.md ~ TASK-038.md    任务规格（含 STATUS 门禁行）
  - scripts/verify/task-000.sh ~ task-038.sh  本地验收脚本（MinGW MSYS2 + vcpkg）

用法：
  python tools/gen/build_tasks.py --check     # 只自检，不写文件
  python tools/gen/build_tasks.py             # 全量生成
  python tools/gen/build_tasks.py --id 013    # 只生成 TASK-013

设计红线：
  1. 不联网、不推送 Git、不写数据库。
  2. --check 发现任何字段缺失 / 依赖不存在 / 依赖成环 / 自依赖，直接非零退出。
  3. 产出的 md 由本脚本全量覆盖，禁止手工编辑（要改就改 data_*.py）。
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

PACK = os.path.dirname(os.path.dirname(HERE))
TASKS_DIR = os.path.join(PACK, "tasks")
VERIFY_DIR = os.path.join(PACK, "scripts", "verify")

import data_a, data_b, data_c, data_d  # noqa: E402

ALL = data_a.TASKS + data_b.TASKS + data_c.TASKS + data_d.TASKS
BY_ID = {t["id"]: t for t in ALL}

# --------------------------------------------------------------------------
# 字段契约
# --------------------------------------------------------------------------
REQUIRED = [
    "id", "name", "phase", "objective", "deps", "module", "owner",
    "inp", "out", "iface", "data", "thread", "hot", "io", "rpc", "persist",
    "files", "steps", "unit", "integ", "bench", "fail",
    "accept", "forbid", "perf", "deliver",
]
OPTIONAL = {
    "ctest": "",
    "bench_bins": [],
    "metrics": [],
    "artifacts": [],
    "scan": [],
    "ports": [],             # 本任务自起服务占用的端口：验收时须「空闲」
    "ports_open": [],        # 外部真实实例端口（Redis/MariaDB 等）：验收时须「已在线监听」
    "content": [],           # [(file, regex, desc)]
    "state": "",             # 该任务专属的 State Owner 描述（缺省用 STATE[id]）
    "ctype": "feat",         # Conventional Commits 类型
    "both_build": False,
}

# --------------------------------------------------------------------------
# State Owner：39 段专属描述，杜绝模板套话
# --------------------------------------------------------------------------
STATE = {
"TASK-000": "仓库与构建配置本身无运行时状态。PROJECT_REQUIREMENTS.md 是冻结规范，Owner 为 @技术总监，变更必须走 RFC + 人工批准；任何 Agent 与开发成员不得直接改写。目录骨架与 CMake 选项由本任务独占定义，后续任务只能在其约束内增量添加。",
"TASK-001": "无运行时状态。Error 与 Result<T> 为纯值类型（值语义、不可变、可跨线程自由传递），不存在 Owner 概念。错误码枚举表由本任务独占维护并集中定义，其他模块只能引用，禁止自定义第二套错误体系。失败路径必须零堆分配。",
"TASK-002": "Logger 不持有任何业务状态。TraceID 由请求发起方（Gateway 入站或 Command 入口）生成并沿调用链透传，本任务只定义载体与传播规则。日志落盘由后台 IO 线程独占，业务线程只做无锁入队，禁止业务线程阻塞等 IO。",
"TASK-003": "配置快照（Config Snapshot）不可变，加载期由 ControlService 独占写入，运行期所有线程只读；版本切换通过原子指针整体替换，禁止原地修改。MonotonicClock 无状态，是 Tick 唯一允许的时间源。UUID 生成器按线程局部无锁实现。",
"TASK-004": "Scheduler 独占 Timer 表与任务队列的写入权，只允许在 Tick Safe Point 或指定 Worker 线程上调度。ObjectPool / Arena / MemoryPool 的 Owner 是使用方线程（每 Worker 独占一份，禁止跨线程共享同一个池）。禁止本层依赖任何 Gameplay 模块。",
"TASK-005": "协议 Schema 是契约资产，不持有运行时状态。Codec 实例无状态、可并发复用。MessageEnvelope 一旦构造即为只读：message_id / message_type / version / source / timestamp_ms / trace_id / request_id / payload 八字段（经济类加 transaction_id + idempotency_key），禁止在传输链路中途改写任一字段。",
"TASK-006": "gRPC Channel Pool 由本任务独占维护（连接生命周期、重连、健康检查、超时）。调用方只持有 stub 句柄，禁止自行创建 channel。RPC 层只做搬运，不持有任何游戏状态，不缓存业务数据。",
"TASK-007": "CommandBus 独占待处理命令队列，EventBus 独占事件订阅表与派发队列；两者都不拥有业务状态，只负责搬运。命令入队后不可变。事件订阅表只允许在模块注册期修改，运行期只读。任何模块不得自定义第二套消息头。",
"TASK-008": "Connection 对象由 Gateway 的网络 IO 线程独占写入（socket 状态、收发缓冲）。业务线程只能提交发送请求，禁止直接触碰 socket。连接表的增删由 Acceptor / Reactor 线程独占，禁止跨线程操作。",
"TASK-009": "Session 的唯一 Owner 是持有该 Session 的 Gateway 实例（SessionStore 独占写）。SessionVersion 单调递增，只有 Gateway 能推进版本；GameNode / DataService 只持有只读缓存副本。Session 内禁止保存玩家最终持久化数据。Suspended 会话必须有 grace 超时释放，禁止无界增长。",
"TASK-010": "路由表（PlayerID → GameNodeID）的 Owner 是 Gateway Router；Redis 中的权威路由由 DataService / ControlService 维护，Gateway 只持有带 TTL 的本地缓存并可自建。GameNode 不拥有任何路由状态，只接受被路由到的请求。",
"TASK-011": "Entity 的生命周期（创建 / 销毁 / 组件挂载）由所属 Scene 在其 Simulation 线程上独占执行，禁止跨线程操作。EntityID 全局唯一且不可复用。组件数据的 Owner 是拥有该组件的系统（Movement 组件归 Movement、Combat 组件归 Combat），禁止跨系统直接写对方组件。",
"TASK-012": "Scene 是实时状态的权威 Owner。SceneID / SceneType / TickNumber / SceneVersion / StateHash 以及 Scene 内所有 Entity 的位置、HP、Buff，只有持有该 Scene 的 GameNode Simulation 线程可写。Redis 与 MySQL 只持有非权威副本。Scene 迁移时 Owner 单点移交，任何时刻禁止双写。",
"TASK-013": "Scheduler 独占 TickNumber 与阶段推进权，只允许 Scene 的 Simulation 线程推进 Tick。Tick 内的八阶段（Input/Movement/AOI/Combat/Buff/Quest/Event/Replication）顺序固定，各系统的写权限由 Scene Owner 在该阶段内授权，同一状态不允许并行写入。Safe Point 只能由 Scheduler 宣告，其他模块不得自行判定。",
"TASK-014": "AOI 索引（格子 → 实体集合）的 Owner 是 AOI System，但索引内容只能在 Movement 阶段由 Scene 统一驱动更新，禁止其他时机写入。可见集（Visible Set）由 AOI 独占计算并缓存，其他模块只读；禁止在 AOI 之外维护第二份可见关系。",
"TASK-015": "玩家坐标 (x, y, z) 与朝向的唯一写入者是所属 Scene 的 Movement System，且只能在 Movement 阶段写入。防加速 / 防穿墙校验必须在写入点完成，禁止绕过 Movement 直接改坐标。客户端上报的坐标是意图，不是权威。",
"TASK-016": "Player / Character 的属性、等级、经验由 Role System 在 Scene Simulation 线程上独占写入，持久化副本归 DataService。实时 HP / MP 的权威在 Scene / Combat，Role 只保存上限与基础属性，禁止把实时 HP 写回 Role 当作权威。Role 不负责 Scene / AOI / Network / MySQL。",
"TASK-017": "背包与装备栏的权威 Owner 是 Inventory System（Scene 线程内）。物品实例 ID 全局唯一、不可复用。任何增删必须生成 Command 并留痕，禁止直接改容器。装备变更触发的属性重算只能在 Scene 线程内完成，禁止异步改属性。",
"TASK-018": "NPC / Monster 实体的状态 Owner 是所属 Scene（与 Player 同规则）。AI 黑板由 AI 模块独占写入，但战斗相关状态（当前目标、仇恨值）由 Combat 写入，AI 只读。NPC 血量归 Scene / Combat，不归 AI。",
"TASK-019": "任务进度（QuestProgress）的 Owner 是 Quest System，只在 Scene 线程内按事件驱动更新，禁止其他模块直接写进度。事件索引（事件类型 → 关心该事件的任务集合）由 Quest 独占维护并常驻内存，用于彻底避免「每秒遍历全服玩家检查所有任务」的反模式。",
"TASK-020": "World / Instance 的生命周期与 Scene 归属由 GameNode 内部的 WorldManager / InstanceManager 独占。实例创建、销毁、玩家进出必须走 Command，禁止直接改世界状态。副本内实时状态的 Owner 仍是该副本的 Scene，不归 World 统一持有。",
"TASK-021": "技能配置（定义表）不可变，归配置系统；技能运行时状态（CD、施法进度、层数、充能）的 Owner 是 Combat System，只在 Scene 线程内写。Skill 不得直接读 Role 私有成员，只能经 Role Command / Interface 访问。",
"TASK-022": "HP 变化（伤害与治疗）的最终结果由 Damage / Heal 模块在 Combat 阶段独占写入。随机数必须来自确定性 PRNG（以 SceneID + TickNumber + 序列号为种子），禁止使用全局随机源，否则无法回放与对账。伤害公式的全部参数必须是配置数据，禁止硬编码在 C++ 里。",
"TASK-023": "Buff 实例的 Owner 是 Buff System，且必须挂在 Scheduler 上按 Tick 结算，禁止自建线程或独立定时器。Buff 引起的属性 / HP 变化仍需经对应 Owner 的 Command 落地，Buff 不得直接改 HP。堆叠、刷新、驱散规则由 Buff 独占判定。",
"TASK-024": "Combat Framework 是战斗流程编排者，独占战斗状态机与当前目标的写权限，但不拥有具体数值：HP 最终写入仍走 Damage / Heal，仇恨表由 Threat 独占。禁止 Combat 直接操作 Role 或 Inventory 的私有成员，一切经 Command / Interface。",
"TASK-025": "Benchmark 是纯观测者，不拥有任何服务状态，被测 Scene 的 Owner 不变（仍是 Scene）。Benchmark 只允许读与统计，禁止在测量过程中改动仿真逻辑或调整参数。所有数字必须原样落盘，禁止后处理、四舍五入美化或删除不达标的组。",
"TASK-026": "DataService 是持久化数据的唯一 Owner 与唯一访问入口，GameNode 禁止直连 MySQL 或 Redis。数据版本号（Version）由 DataService 独占推进，乐观锁冲突以 DataService 的判定为准。IRepository 接口契约由本任务独占定义。",
"TASK-027": "Redis 只保存非权威、可重建的数据（Session 缓存、路由缓存、排行榜、临时数据），写入权归 DataService。任何键都必须可过期或可从 MySQL 重建，禁止把 Redis 当作最终权威，禁止把 RDB Snapshot 当作唯一故障恢复方案。禁止 KEYS 命令。",
"TASK-028": "MySQL 是最终持久化权威。分片路由（shard 选择）由 DataService 内部的分片策略独占决定，业务层禁止感知分片数、禁止写死 8。Schema 迁移只能由本任务提供的迁移工具执行，禁止手工改表。密码只存 salted hash，禁止明文或可逆加密。",
"TASK-029": "货币与物品余额的内存权威 Owner 是 Economy System（Scene 线程内），持久化权威归 DataService。所有余额变更必须走 EconomyCommand，禁止任何模块直接加减货币字段。查询走 Query，禁止产生副作用。",
"TASK-030": "Ledger 是 append-only 的不可变事实，写入后禁止修改或删除（更正只能追加冲正条目）。哈希链（prev_hash → hash）由写入方独占计算并可校验。幂等键状态机（Fresh / InFlight / Completed / Failed）的 Owner 是本任务，只能在 Scene 线程内推进状态，数据库 UNIQUE 索引作为最终兜底。",
"TASK-031": "每个 Scene 独占一个 LuaVM，禁止全局单例、禁止跨线程共享 VM。VM 内全局表归该 Scene 的脚本层，C++ 侧不得长期持有裸 Lua 引用（必须经 Registry / 引用池管理）。沙箱白名单与四类限额（内存 / 指令 / 时间 / 栈深）由本任务独占维护。",
"TASK-032": "脚本版本（ScriptVersion / ConfigVersion）的 Owner 是 Hot Reload Manager。激活只允许在 Tick Safe Point，而 Safe Point 由 Scheduler 宣告，Manager 不得自行判定安全点，更不得在 Tick 执行中途替换脚本。回滚窗口内保留最近 5 个版本，回滚权归 Manager。",
"TASK-033": "玩法脚本不拥有游戏状态，只读取宿主（Scene / Combat / Quest）经接口传入的数据并回传决策。所有对游戏状态的写必须经过 Command，脚本禁止直写 C++ 内存。脚本可写的只有自身局部变量与黑板。C++ 负责 Simulation / Entity / Memory / Scheduler / Network / AOI / 核心战斗框架，Lua 只负责规则与公式。",
"TASK-034": "客户端持有的是服务端状态的非权威预测副本（插值 + 本地预测）。服务端快照到达后必须纠正（reconciliation），客户端不得认为自己拥有任何权威状态。客户端输入只是意图上报，不是权威结果。客户端只复用 TASK-005 的协议，禁止依赖任何服务端模块。",
"TASK-035": "渲染层不拥有游戏状态，只做只读呈现。渲染资源（材质、网格、贴图、着色器）的生命周期由 Renderer 独占管理，禁止业务层直接 new / delete GPU 资源。画质档位（Low / Medium / High）由本任务独占维护，支持运行时切换。",
"TASK-036": "资源加载状态（已加载 Chunk 集合、加载队列、卸载队列）的 Owner 是 Resource Manager，运行在独立加载线程上，通过无锁队列与主线程通信。Current Chunk + Nearby Chunk 的集合判定与迟滞防抖规则由本任务独占，禁止渲染层自行加载任意资源。",
"TASK-037": "故障检测权归 Health Monitor（Gateway 侧），会话冻结与重连编排权归 Gateway，玩家状态恢复的权威数据源是 DataService 的持久化快照。迁移期间禁止双写：旧 GameNode 未完成冻结前，新 GameNode 不得接受写入。本阶段明确不实现 Live Scene Migration。",
"TASK-038": "压测 / Chaos / 对账工具是纯观测与施压方，不拥有任何服务状态。容量报告中的数字必须来自真实执行结果，未达标必须如实记录容量上限并给出瓶颈定位，禁止用估算值、理论值或更高配置机器的数据替代。",
"TASK-039": "社交状态（组队/好友/公会/邮件）的运行时权威归 Social System（GameNode 内，单节点内全量内存态）；跨节点社交由 Gateway 路由 + 持久化副本兜底。公会/邮件的持久化权威归 DataService。社交事件（邀请/入会/私聊）走 EventBus，禁止直接改对方 Role 或 Scene 数据。",
"TASK-040": "ControlService 是集群控制面：节点注册表（NodeRegistry 的权威同步方）、配置版本（Config Version）下发、健康检查/运维接口、Gateway 多实例注册的权威。它不拥有任何游戏状态，只做控制与协调；灰度发布与自动扩缩容列为 Phase 2。",
"TASK-041": "集成与回归任务不拥有任何服务状态，是纯验证方。它消费 TASK-025 的战斗性能矩阵定义、TASK-030 的账本对账、TASK-033 的 Lua 脚本全集，做跨进程（gRPC + Redis + MySQL）端到端演练与「Lua 落地后的战斗性能回归」。禁止在回归中改动被验证模块的实现。",
}

# --------------------------------------------------------------------------
# 模块边界 / 接口契约 / 扩展性（全任务统一，防相互干扰、保可扩展）
# --------------------------------------------------------------------------
BOUNDARY_RULES = [
    "模块 ≠ 进程：本任务代码只落在自身 `module` 子树（`include/` + `src/` + `tests/` + `docs/`），"
    "禁止扩散到其它任务拥有的目录。",
    "下游只能通过本任务 `include/` 下的**公开头与接口**调用，禁止 `#include` 本任务 `src/` 或内部头"
    "（验收脚本会静态扫描本任务 `include/` 是否泄露内部 `src/`）。",
    "本任务只调用依赖模块**声明**的接口，禁止访问其内部数据（如 `otherModule.internalData` 模式）。",
    "接口在 `STATUS: DONE` 之后变更必须走 `version` 字段 + 兼容性评估，禁止静默改签名导致下游编译失败。",
    "依赖方向单向（Game → Gameplay → Core），禁止循环依赖；新增模块不得破坏既有依赖环约束。",
]

EXTENSIBILITY_RULES = [
    "新增同类能力（新 Command / 新 Event / 新 Scene 类型 / 新模块）必须走**注册表 / ID 段**机制，"
    "禁止在 `switch` 里硬编码穷举。",
    "跨模块扩展点统一用抽象（C++ Interface / Command / Event），新增实现**不得修改既有任务文件**。",
    "协议 / 接口变更必须带 `version` 字段并向下兼容，旧客户端 / 旧模块不得因此断连或编译失败。",
    "所有模块遵循统一目录模板（include/src/tests/benchmark/docs/CMakeLists.txt）与五文档契约"
    "（README/INTERFACE/DEPENDENCY/PERFORMANCE/TEST），新增模块不得例外。",
]

# --------------------------------------------------------------------------
# 辅助函数
# --------------------------------------------------------------------------
def dep_ids(deps_str):
    """把 'TASK-003, TASK-004' 解析为 ['003', '004']。"""
    if not deps_str or deps_str.strip() in ("无", "-", ""):
        return []
    return [m for m in re.findall(r"TASK-(\d{3})", deps_str)]


def flag(v):
    """从 'YES（xxx）' 中拆出 (YES, '（xxx）')。"""
    s = (v or "").strip()
    m = re.match(r"^(YES|NO|N/A|部分)\s*(.*)$", s)
    if m:
        return m.group(1), m.group(2)
    return s, ""


def sq(s):
    """生成 bash 单引号字面量。"""
    return "'" + str(s).replace("'", "'\\''") + "'"


def bullets(items, ordered=False):
    out = []
    for i, it in enumerate(items, 1):
        prefix = "%d." % i if ordered else "-"
        out.append("%s %s" % (prefix, it))
    return "\n".join(out)


def concrete(paths):
    """过滤掉带通配符或中文说明的条目，只留可 stat 的具体路径。"""
    res = []
    for p in paths:
        if "*" in p or "？" in p or "（" in p or "(" in p:
            continue
        res.append(p)
    return res


def consumes_bullets(t):
    """本任务消费的上游接口清单（从依赖推导，防止绕过接口直戳内部）。"""
    out = []
    for d in dep_ids(t.get("deps", "")):
        dt = BY_ID.get("TASK-%s" % d)
        if not dt:
            continue
        out.append("- `%s` · `%s`：消费其 `include/` 下公开接口（详见该任务 §7 Public Interface），"
                   "禁止 `#include` 其 `src/`" % (dt["id"], dt["module"]))
    if not out:
        out.append("- 无前置依赖，不消费任何上游接口。")
    return "\n".join(out)


# --------------------------------------------------------------------------
# 自检
# --------------------------------------------------------------------------
def check(tasks):
    errs = []
    ids = [t["id"] for t in tasks]

    if len(ids) != len(set(ids)):
        dup = [i for i in set(ids) if ids.count(i) > 1]
        errs.append("TASK-ID 重复：%s" % dup)

    # 编号连续性
    expect = ["TASK-%03d" % i for i in range(len(tasks))]
    if sorted(ids) != sorted(expect):
        errs.append("TASK-ID 编号不连续/数量异常：期望 %d 个 TASK-000..TASK-%03d，实得 %s"
                    % (len(tasks), len(tasks) - 1, sorted(ids)))

    graph = {}
    for t in tasks:
        tid = t["id"]
        missing = [k for k in REQUIRED if k not in t or (isinstance(t[k], str) and not t[k].strip())]
        if missing:
            errs.append("%s 缺失必填字段：%s" % (tid, missing))
        unknown = [k for k in t if k not in REQUIRED and k not in OPTIONAL]
        if unknown:
            errs.append("%s 存在未契约字段：%s" % (tid, unknown))
        ds = dep_ids(t.get("deps", ""))
        for d in ds:
            full = "TASK-%s" % d
            if full not in ids:
                errs.append("%s 依赖不存在的任务：%s" % (tid, full))
            if full == tid:
                errs.append("%s 自依赖" % tid)
        graph[tid] = ds

    # DFS 三色标记检测环
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {tid: WHITE for tid in ids}
    stack_path = []

    def dfs(u):
        color[u] = GRAY
        stack_path.append(u)
        for v in graph.get(u, []):
            full = "TASK-%s" % v
            if full not in color:
                continue
            if color[full] == GRAY:
                loop = stack_path[stack_path.index(full):] + [full]
                errs.append("依赖成环：%s" % " → ".join(loop))
            elif color[full] == WHITE:
                dfs(full)
        stack_path.pop()
        color[u] = BLACK

    for tid in ids:
        if color[tid] == WHITE:
            dfs(tid)

    # State Owner 必须存在且非模板
    for t in tasks:
        st = (t.get("state") or STATE.get(t["id"]) or "").strip()
        if not st:
            errs.append("%s 缺少 State Owner 描述" % t["id"])
        elif len(st) < 30:
            errs.append("%s State Owner 描述过短（疑似模板套话）：%s" % (t["id"], st))

    # State Owner 唯一性
    texts = [(t["id"], (t.get("state") or STATE.get(t["id"]) or "")) for t in tasks]
    seen = {}
    for tid, txt in texts:
        if txt in seen:
            errs.append("State Owner 雷同：%s 与 %s" % (seen[txt], tid))
        seen[txt] = tid

    # Phase 单调性（不允许 Phase 号回退造成依赖倒置）
    # 例外：最终交付/集成任务（无任何其它任务依赖它的「汇点」）可合法依赖所有阶段，跳过自检。
    dependents = set()
    for t in tasks:
        for d in dep_ids(t.get("deps", "")):
            dependents.add("TASK-%s" % d)
    sink_ids = [t["id"] for t in tasks if t["id"] not in dependents]

    def phase_no(t):
        m = re.search(r"Phase\s+(\d+)", t.get("phase", ""))
        return int(m.group(1)) if m else 999
    for t in tasks:
        if t["id"] in sink_ids:
            continue
        pn = phase_no(t)
        for d in dep_ids(t.get("deps", "")):
            dt = BY_ID.get("TASK-%s" % d)
            if dt and phase_no(dt) > pn:
                errs.append("%s(Phase %d) 依赖了更晚的 %s(Phase %d)，疑似依赖倒置"
                            % (t["id"], pn, dt["id"], phase_no(dt)))
    return errs


# --------------------------------------------------------------------------
# 渲染 Markdown
# --------------------------------------------------------------------------
def render_md(t):
    tid = t["id"]
    num = tid.split("-")[1]
    deps = dep_ids(t.get("deps", ""))
    dep_full = ["TASK-%s" % d for d in deps]
    state = (t.get("state") or STATE.get(tid) or "").strip()
    ctype = t.get("ctype", "feat")
    hot_f, hot_n = flag(t["hot"])
    io_f, io_n = flag(t["io"])
    rpc_f, rpc_n = flag(t["rpc"])
    per_f, per_n = flag(t["persist"])

    head = []
    head.append("---")
    head.append("TASK-ID: %s" % tid)
    head.append("NAME: %s" % t["name"])
    head.append("PHASE: %s" % t["phase"])
    head.append("MODULE: %s" % t["module"])
    head.append("OWNER: %s" % t["owner"])
    head.append("STATUS: PENDING")
    head.append("DEPENDENCIES: %s" % (", ".join(dep_full) if dep_full else "无"))
    head.append("---")
    head.append("")
    head.append("# %s · %s" % (tid, t["name"]))
    head.append("")
    head.append("> 本文件由 `tools/gen/build_tasks.py` 从 `tools/gen/data_*.py` 生成，**禁止手工编辑**。")
    head.append("> 需要改动请修改数据源后重新生成：`python tools/gen/build_tasks.py`")
    head.append("")
    head.append("| 字段 | 值 |")
    head.append("|---|---|")
    head.append("| TASK-ID | `%s` |" % tid)
    head.append("| NAME | %s |" % t["name"])
    head.append("| PHASE | %s |" % t["phase"])
    head.append("| MODULE | `%s` |" % t["module"])
    head.append("| OWNER | %s |" % t["owner"])
    head.append("| STATUS | **PENDING** |")
    head.append("| DEPENDENCIES | %s |" % (", ".join("`%s`" % d for d in dep_full) if dep_full else "无"))
    head.append("")
    head.append("---")
    head.append("")

    s = []
    s.append("## 1. Objective")
    s.append("")
    s.append(t["objective"].strip())
    s.append("")

    s.append("## 2. Dependencies")
    s.append("")
    if dep_full:
        s.append("### 2.1 前置任务")
        s.append("")
        for d in dep_full:
            dt = BY_ID.get(d)
            nm = dt["name"] if dt else "（未知）"
            s.append("- `%s` · %s" % (d, nm))
        s.append("")
        s.append("### 2.2 门禁规则")
        s.append("")
        s.append("验收脚本会先执行 `require_tasks_done %s`：" % " ".join(deps))
        s.append("任一前置任务的 `STATUS` 不是 `DONE`，脚本立即非零退出，**禁止越级实施**。")
    else:
        s.append("无前置任务（本任务为 Phase 起点）。")
    s.append("")

    s.append("## 3. Module")
    s.append("")
    s.append("`%s`" % t["module"])
    s.append("")

    s.append("## 4. State Owner（状态归属）")
    s.append("")
    s.append(state)
    s.append("")
    s.append("> 硬约束：同一实时状态只能有一个权威写入者（见 PROJECT_REQUIREMENTS §10 / §12）。")
    s.append("> 跨模块写入必须走 Command，禁止直接改对方内存。")
    s.append("")

    s.append("## 5. Input")
    s.append("")
    s.append(t["inp"].strip())
    s.append("")

    s.append("## 6. Output")
    s.append("")
    s.append(t["out"].strip())
    s.append("")

    s.append("## 7. Public Interface")
    s.append("")
    s.append(t["iface"].strip())
    s.append("")

    s.append("## 8. Data Model")
    s.append("")
    s.append(t["data"].strip())
    s.append("")

    s.append("## 9. Thread Model")
    s.append("")
    s.append(t["thread"].strip())
    s.append("")

    s.append("## 10. Hot Path")
    s.append("")
    s.append("**%s**%s" % (hot_f, (" " + hot_n) if hot_n else ""))
    s.append("")
    if hot_f == "YES":
        s.append("本任务位于 Tick 热路径内，禁止：MySQL / Redis 同步访问 / 同步 gRPC / Kafka 同步访问 / 文件 IO / "
                 "网络阻塞 IO / 大规模内存分配（见 PROJECT_REQUIREMENTS §14）。")
    s.append("")

    s.append("## 11. External IO")
    s.append("")
    s.append("**%s**%s" % (io_f, (" " + io_n) if io_n else ""))
    s.append("")
    if io_f == "YES":
        s.append("所有外部 IO 必须异步化，禁止出现在 Tick 内。")
    s.append("")

    s.append("## 12. Network RPC")
    s.append("")
    s.append("**%s**%s" % (rpc_f, (" " + rpc_n) if rpc_n else ""))
    s.append("")
    if rpc_f == "YES":
        s.append("跨进程统一 gRPC + Protobuf；热路径禁止依赖远程 RPC（见 PROJECT_REQUIREMENTS §5.3）。")
    s.append("")

    s.append("## 13. Persistence")
    s.append("")
    s.append("**%s**%s" % (per_f, (" " + per_n) if per_n else ""))
    s.append("")
    if per_f == "YES":
        s.append("持久化只能经 DataService，禁止 GameNode 直连 MySQL / Redis。")
    s.append("")

    s.append("## 14. Files")
    s.append("")
    s.append(bullets(t["files"]))
    s.append("")

    s.append("## 15. Implementation Steps")
    s.append("")
    s.append(bullets(t["steps"], ordered=True))
    s.append("")

    s.append("## 16. Unit Test")
    s.append("")
    s.append(t["unit"].strip())
    s.append("")

    s.append("## 17. Integration Test")
    s.append("")
    s.append(t["integ"].strip())
    s.append("")

    s.append("## 18. Benchmark")
    s.append("")
    s.append(t["bench"].strip())
    s.append("")

    s.append("## 19. Failure Test")
    s.append("")
    s.append(t["fail"].strip())
    s.append("")

    s.append("## 20. Acceptance Criteria")
    s.append("")
    s.append(bullets(t["accept"], ordered=True))
    s.append("")
    s.append("以上每一条都必须在本地真实执行并留证；**任一条不满足即判定本任务未完成**，禁止进入下一个 TASK。")
    s.append("")

    s.append("## 21. Forbidden")
    s.append("")
    s.append(bullets(t["forbid"]))
    s.append("")
    s.append("> 统一边界红线（全任务适用，详见 §27.3）：禁止扩散到他人 `module` 子树；"
             "下游禁止 `#include` 本任务 `src/`；禁止访问依赖模块内部数据；"
             "禁止在 `STATUS: DONE` 后静默改接口签名；禁止循环依赖。")
    s.append("")

    s.append("## 22. Performance Expectation")
    s.append("")
    s.append(t["perf"].strip())
    s.append("")

    s.append("## 23. Deliverables")
    s.append("")
    s.append(bullets(t["deliver"]))
    s.append("")

    # ---- 24 验收脚本 -----------------------------------------------------
    s.append("## 24. Verification Script（本地验收）")
    s.append("")
    s.append("**验收脚本**：`scripts/verify/task-%s.sh`（由生成器产出，禁止手工编辑）" % num)
    s.append("")
    s.append("```bash")
    s.append("# 默认 Release；可指定 Debug：BUILD_TYPE=Debug bash scripts/verify/task-%s.sh" % num)
    s.append("bash scripts/verify/task-%s.sh" % num)
    s.append("```")
    s.append("")
    checks = []
    if dep_full:
        checks.append("前置任务门禁：`require_tasks_done %s`" % " ".join(deps))
    # 交付物存在性检查取「关键交付物 artifacts」与「§8 交付物清单 deliver」的并集：
    # 早期用 `artifacts or deliver`（二选一）导致给了 artifacts 的任务只检查 1~3 项，
    # 其余交付物缺失也不报错（如 TASK-010 只查 1/12、TASK-028 只查 2/6）——属静默漏检。
    fc = concrete(list(dict.fromkeys(list(t.get("artifacts") or []) + t["deliver"])))
    if fc:
        checks.append("交付物存在性检查（%d 项）" % len(fc))
    for f, pat, desc in t.get("content", []):
        checks.append("内容检查：`%s` 必须匹配 /%s/" % (f, pat))
    for d, pat in t.get("scan", []):
        checks.append("静态红线扫描：`%s` 内禁止出现 /%s/" % (d, pat))
    for p in t.get("ports", []):
        checks.append("端口空闲检查：%s（本任务自起服务须可用）" % p)
    for p in t.get("ports_open", []):
        checks.append("端口在线检查：%s（须有真实外部实例在监听）" % p)
    checks.append("CMake configure + 编译（%s）" % ("Debug + Release 双构建" if t.get("both_build") else "Release 单构建"))
    if t.get("ctest"):
        checks.append("ctest 过滤执行：`-R %s`" % t["ctest"])
    for exe, args in t.get("bench_bins", []):
        checks.append("Benchmark 执行：`%s %s`" % (exe, args))
    for f, k, op, thr in t.get("metrics", []):
        opname = {"le": "≤", "lt": "<", "ge": "≥"}.get(op, op)
        checks.append("性能阈值断言：`%s` 中 `%s` %s `%s`" % (f, k, opname, thr))
    s.append("脚本执行的检查项：")
    s.append("")
    s.append(bullets(checks, ordered=True))
    s.append("")
    s.append("脚本遵循 `set -euo pipefail`：任一步失败即非零退出，**不存在「警告通过」**。")
    s.append("脚本只报告真实执行结果，禁止兜底伪造 PASS；指标缺失直接判失败，禁止用估算值代替。")
    s.append("")

    # ---- 25 Git Commit ---------------------------------------------------
    s.append("## 25. Git Commit")
    s.append("")
    s.append("**必须先通过验收脚本（退出码 0），才允许提交。**")
    s.append("")
    s.append("```bash")
    s.append("# 1) 把本任务标记为 DONE（脚本会校验 STATUS 流转合法）")
    s.append("bash scripts/task-done.sh %s" % tid)
    s.append("")
    s.append("# 2) 提交：Conventional Commits，scope 用模块名")
    s.append('git add -A')
    s.append('git commit -F - <<\'EOF\'')
    s.append("%s(%s): %s" % (ctype, short_scope(t["module"]), t["name"]))
    s.append("")
    s.append("- 实现要点：（填写本任务实际落地的内容，禁止复制 Objective）")
    s.append("- 实测数字：（粘贴 scripts/verify/task-%s.sh 的真实输出，禁止写「性能良好」）" % num)
    s.append("")
    s.append("Refs: %s" % tid)
    s.append("EOF")
    s.append("")
    s.append("# 3) 推送：GFW 屏蔽 ssh.github.com，必须走 22 端口")
    s.append("git push git@github.com:22:shengmingaini/CAMI.git main")
    s.append("```")
    s.append("")
    s.append("提交规范：")
    s.append("")
    s.append("- 类型：`feat` / `fix` / `perf` / `refactor` / `test` / `docs` / `build` / `chore`（本任务建议 `%s`）" % ctype)
    s.append("- **一个 TASK 一次独立提交**，禁止把多个 TASK 合并提交")
    s.append("- 正文必须包含实测数字，禁止「性能良好 / 已优化」这类无法验证的描述")
    s.append("- 未通过验收脚本禁止提交，禁止 `--no-verify` 绕过")
    s.append("")

    # ---- 26 执行原则 -----------------------------------------------------
    s.append("## 26. Codex Execution Rules")
    s.append("")
    rules = [
        "读规范：先读 `PROJECT_REQUIREMENTS.md` 与本任务涉及章节，架构冻结，不得自行推翻。",
        "读任务：完整读完本文件全部章节再动手，禁止只看 Objective 就开始写。",
        "查依赖：确认 %s 均已 `STATUS: DONE`，否则停止并报告。" % (", ".join(dep_full) if dep_full else "无前置"),
        "查现状：grep 现有代码，确认要改的文件与符号真实存在，禁止凭空假设。",
        "守范围：只改本任务 §14 Files 范围内的文件；发现范围外问题只记录不修改。",
        "做实现：按 §15 Implementation Steps 顺序落地，每步可独立编译。",
        "本地编译：MinGW MSYS2 g++ + vcpkg manifest mode（baseline `aae277ac`），Debug 与 Release 都要过。",
        "跑单测：§16 Unit Test 全绿，新增代码必须带测试。",
        "跑集成：§17 Integration Test 全绿。",
        "跑 Benchmark：§18 真实执行，输出机器可读的 `key=value`，禁止估算。",
        "出结果：把实测数字写入 §22 对应的报告文件，不达标如实记录。",
        "跑验收脚本：`bash scripts/verify/task-%s.sh` 退出码 0 后，才执行 §25 提交。" % num,
    ]
    s.append(bullets(rules, ordered=True))
    s.append("")
    s.append("> **门禁**：本任务未通过 §20 Acceptance Criteria 与 §24 验收脚本，禁止进入下一个 TASK。")
    s.append("")

    s.append("## 27. 接口契约、模块边界与扩展性")
    s.append("")
    s.append("本节是**防任务间交付相互干扰 + 保框架可扩展**的统一契约，所有任务适用。")
    s.append("")
    s.append("### 27.1 本任务导出的接口（冻结后不可破坏性变更）")
    s.append("")
    s.append("见 §7 Public Interface。导出头只放在本任务 `include/` 下，签名一旦 `STATUS: DONE` "
             "即视为契约冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估。")
    s.append("")
    s.append("### 27.2 本任务消费的上游接口（来自前置任务，禁止绕过）")
    s.append("")
    s.append(consumes_bullets(t))
    s.append("")
    s.append("### 27.3 模块边界红线（全任务统一）")
    s.append("")
    s.append(bullets(BOUNDARY_RULES))
    s.append("")
    s.append("### 27.4 扩展性约束（可扩展框架兼容性）")
    s.append("")
    s.append(bullets(EXTENSIBILITY_RULES))
    s.append("")
    s.append("> 模块归属表（谁拥有哪棵子树）：本任务的 `module` 字段即其独占目录；")
    s.append("> 其它任务的 `module` 字段不得被本任务写入。统一模块模板见 `DEVELOPMENT.md` / 根规范 §6。")
    s.append("")

    s.append("## 28. 变更记录")
    s.append("")
    s.append("| 日期 | 变更 |")
    s.append("|---|---|")
    s.append("| 2026-08-29 | 方案 A 原地补齐：由 `tools/gen/build_tasks.py` 从结构化数据源重新生成，"
             "补齐 State Owner / 验收脚本 / STATUS 门禁 / Git Commit 规范 |")
    s.append("| 2026-08-29 | 完善：新增 §27 接口契约/模块边界/扩展性（全任务统一，防相互干扰）；"
             "新增 TASK-039 Social / TASK-040 ControlService / TASK-041 集成与回归；"
             "依赖相位自检跳过最终交付汇点；Scene Migration 登记为 Phase 2 RFC |")
    s.append("")

    return "\n".join(head + s)


def short_scope(module):
    m = re.split(r"[/., (（]", module)[0]
    m = m.strip().lower()
    mapping = {
        "build": "build", "repo": "build", "engine": "core", "core": "core",
        "protocol": "protocol", "server": "server", "benchmark": "bench",
        "scripting": "script", "client": "client", "tools": "tools",
        "database": "db", "config": "config", "docs": "docs",
    }
    return mapping.get(m, m or "core")


# --------------------------------------------------------------------------
# 渲染验收脚本
# --------------------------------------------------------------------------
def render_sh(t):
    tid = t["id"]
    num = tid.split("-")[1]
    deps = dep_ids(t.get("deps", ""))

    L = []
    L.append("#!/usr/bin/env bash")
    L.append("# TASK-%s · %s —— 本地验收脚本" % (num, t["name"]))
    L.append("# 自动生成：python tools/gen/build_tasks.py   （禁止手工编辑，改数据源后重新生成）")
    L.append("# 环境：Windows Git Bash / MSYS2 MinGW；g++ (MinGW MSYS2)；vcpkg manifest mode baseline aae277ac")
    L.append("# 用法：bash scripts/verify/task-%s.sh" % num)
    L.append("#      BUILD_TYPE=Debug  bash scripts/verify/task-%s.sh" % num)
    L.append("# 红线：不做网络操作、不推送 Git、不写数据库；任一步失败即非零退出。")
    L.append("")
    L.append('ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"')
    L.append('source "$ROOT/scripts/verify/_common.sh"')
    L.append("")
    L.append('begin_task "%s" %s' % (tid, sq(t["name"])))
    L.append("")

    if deps:
        L.append("# ---- 1. 前置任务门禁：必须全部 STATUS: DONE ----")
        L.append("require_tasks_done %s" % " ".join(deps))
        L.append("")
        i = 2
    else:
        L.append("# ---- 1. 前置任务门禁：无前置 ----")
        L.append('info "本任务无前置依赖，跳过门禁"')
        L.append("")
        i = 2

    # 同 §20：存在性检查覆盖 artifacts ∪ deliver（去重保序），避免只查少数关键产物。
    files = concrete(list(dict.fromkeys(list(t.get("artifacts") or []) + t["deliver"])))
    if files:
        L.append("# ---- %d. 交付物存在性 ----" % i)
        L.append("require_files \\")
        for j, f in enumerate(files):
            tail = "" if j == len(files) - 1 else " \\"
            L.append("  %s%s" % (sq(f), tail))
        L.append("")
        i += 1

    for f, pat, desc in t.get("content", []):
        L.append("# ---- %d. 交付物内容检查 ----" % i)
        L.append("require_content %s %s %s" % (sq(f), sq(pat), sq(desc)))
        L.append("")
        i += 1

    if t.get("scan"):
        L.append("# ---- %d. 静态红线扫描 ----" % i)
        for d, pat in t["scan"]:
            L.append("scan_forbidden %s %s" % (sq(d), sq(pat)))
        L.append("")
        i += 1

    # 模块边界扫描：本任务的公开头（include/）不得泄露内部 src/ —— 防止下游被迫依赖内部实现
    if "/" in t["module"]:
        inc_dir = t["module"] + "/include"
        L.append("# ---- %d. 模块边界：公开头不得 include 内部 src/ ----" % i)
        L.append('if [ -d "$ROOT/%s" ]; then' % inc_dir)
        L.append('  scan_forbidden %s %s' % (sq(inc_dir), sq(r'#include\s+["<][^">]*src/[^">]*')))
        L.append("fi")
        L.append("")
        i += 1

    if t.get("ports"):
        L.append("# ---- %d. 端口空闲检查（本任务自起服务：端口须可用） ----" % i)
        for p in t["ports"]:
            L.append("require_free_port %s" % p)
        L.append("")
        i += 1

    if t.get("ports_open"):
        L.append("# ---- %d. 端口在线检查（真实外部实例：端口须已在线监听，§20.1）----" % i)
        L.append("#    语义与 require_free_port 相反：真实实例类任务要求端口被实例占用而非空闲。")
        for p in t["ports_open"]:
            L.append("require_port_open %s" % p)
        L.append("")
        i += 1

    L.append("# ---- %d. 编译（本地 MinGW + vcpkg，CI 不作为验收依据） ----" % i)
    if t.get("both_build"):
        L.append("cmake_build_both")
    else:
        L.append("cmake_configure \"$BUILD_TYPE\"")
        L.append("cmake_build \"$BUILD_TYPE\"")
    L.append("")
    i += 1

    if t.get("ctest"):
        L.append("# ---- %d. 单元测试（ctest 过滤执行） ----" % i)
        L.append('run_ctest "$BUILD_TYPE" %s %s' % (sq(t["ctest"]), sq(t["ctest"])))
        L.append("")
        i += 1

    if t.get("bench_bins") or t.get("metrics"):
        L.append("# ---- %d. Benchmark 与性能阈值断言 ----" % i)
        L.append('mkdir -p "$ROOT/bench"')
        for exe, args in t.get("bench_bins", []):
            L.append("run_bench \"$BUILD_TYPE\" %s %s" % (sq(exe), args if args else ""))
        for f, k, op, thr in t.get("metrics", []):
            L.append("assert_metric %s %s %s %s" % (sq(f), sq(k), sq(op), sq(thr)))
        L.append("")
        i += 1

    L.append("# ---- %d. 验收结论 ----" % i)
    L.append('info "人工复核项（脚本无法自动判定，必须人工确认后勾选）："')
    for idx, a in enumerate(t["accept"], 1):
        L.append('info "  [ ] %d. %s"' % (idx, a.replace('"', '')))
    L.append("")
    L.append('end_task "%s"' % tid)
    L.append("")
    return "\n".join(L)


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只做自检，不写文件")
    ap.add_argument("--id", default="", help="只生成指定任务，如 013 或 TASK-013")
    args = ap.parse_args()

    tasks = ALL
    if args.id:
        key = args.id if args.id.startswith("TASK-") else "TASK-%s" % args.id.zfill(3)
        tasks = [t for t in ALL if t["id"] == key]
        if not tasks:
            print("找不到任务：%s" % key)
            return 1

    errs = check(tasks)
    if errs:
        print("=" * 70)
        print("自检失败，共 %d 项：" % len(errs))
        for e in errs:
            print("  ✗ %s" % e)
        print("=" * 70)
        return 1

    print("自检通过：%d 个任务，字段完整、依赖闭合、无环、无自依赖、State Owner 全唯一。"
          % len(tasks))

    if args.check:
        return 0

    os.makedirs(TASKS_DIR, exist_ok=True)
    os.makedirs(VERIFY_DIR, exist_ok=True)

    n_md = n_sh = 0
    for t in tasks:
        num = t["id"].split("-")[1]
        md_path = os.path.join(TASKS_DIR, "TASK-%s.md" % num)
        with open(md_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(render_md(t))
        n_md += 1

        sh_path = os.path.join(VERIFY_DIR, "task-%s.sh" % num)
        with open(sh_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(render_sh(t))
        try:
            os.chmod(sh_path, 0o755)
        except OSError:
            pass
        n_sh += 1

    print("生成完成：%d 份任务规格 → tasks/，%d 个验收脚本 → scripts/verify/" % (n_md, n_sh))
    return 0


if __name__ == "__main__":
    sys.exit(main())
