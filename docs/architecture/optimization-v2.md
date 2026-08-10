# CAMI 架构优化设计文档 — 对标WoW差距补齐

> **文档状态**: [PRODUCTION]  
> **版本**: v2.0.0  
> **更新日期**: 2026-08-06  
> **符合5万在线架构**: 是  
> **变更范围**: 基于CAMI vs WoW对标分析，补齐3个P0架构遗漏 + 4个P1能力差距

---

## 0. 优化总览

### 0.1 优化项清单

| # | 优化项 | 优先级 | 影响模块 | 对标WoW差距 |
|---|--------|--------|---------|------------|
| 1 | 客户端预测与纠偏系统 | P0 | Gateway + GameNode(新增) | 完全缺失，影响基础网络体验 |
| 2 | 寻路与碰撞系统 | P0 | GameNode(新增) | 完全缺失，MMO核心基础设施 |
| 3 | AOI LOD分级更新 | P0 | GameNode(AOI增强) | 缺少生产级优化 |
| 4 | 场景Phasing/Layering | P1 | GameNode(场景增强) | 缺少分层/相位概念 |
| 5 | 法术批次处理 + LOS缓存 | P1 | GameNode(战斗增强) | 缺少公平性保障和性能优化 |
| 6 | 高频消息位压缩 | P1 | Gateway(协议增强) | FlatBuffers带宽开销过大 |

### 0.2 架构变更摘要

```
新增模块:
  game/prediction/     — 客户端预测与纠偏系统
  game/navigation/     — NavMesh寻路系统
  game/collision/      — 碰撞检测与LOS缓存
  game/scene/phasing/  — 场景相位/分层管理

增强模块:
  game/aoi/            — LOD三级更新 + 属性压缩 + 视野锥裁剪
  game/combat/         — Spell Batching + LOS查询接口
  gateway/codec/       — 高频消息位压缩编解码器
  game/scene/          — Phasing/Layering调度
```

### 0.3 性能影响预估

| 指标 | 优化前 | 优化后 | 变化 |
|------|--------|--------|------|
| 高频消息带宽 | ~4.2Gbps (FlatBuffers) | ~1.6Gbps (位压缩) | -62% |
| 远距离玩家更新频率 | 每帧 (33ms) | 最远500ms一次 | -93% |
| 战斗循环LOS查询 | 每次计算 ~0.1ms | 缓存命中 ~0.001ms | -99% |
| 移动体验延迟感知 | 无预测，直接服务器确认 | 客户端预测0ms感知 | 质变 |
| 寻路计算 | 无（缺失） | NavMesh ~0.5ms/次 | 新增能力 |

---

## 1. 客户端预测与纠偏系统 (P0)

### 1.1 设计目标

在不牺牲服务器权威性的前提下，消除网络延迟对玩家操作体验的影响。客户端预测让玩家操作"零延迟"生效，服务器纠偏保证数据一致性。

### 1.2 核心架构

```
┌─────────────────────────────────────────────────────────────┐
│                     客户端                                    │
│  ┌──────────┐   ┌──────────────┐   ┌──────────────────┐    │
│  │ 输入采集  │──→│ 预测模拟器    │──→│ 渲染插值器        │    │
│  │          │   │ (本地物理执行) │   │ (其他玩家平滑插值) │    │
│  └──────────┘   └──────┬───────┘   └──────────────────┘    │
│                        │                                      │
│                 ┌──────▼───────┐                              │
│                 │ 纠偏 reconciler│                             │
│                 │ (对比服务器状态)│                             │
│                 └──────┬───────┘                              │
└────────────────────────┼──────────────────────────────────────┘
                         │ TCP (可靠消息)
                         │ UDP (高频位置)
┌────────────────────────┼──────────────────────────────────────┐
│                    Gateway                                    │
│  ┌──────────┐   ┌──────▼───────┐   ┌──────────────────┐      │
│  │ 协议编解码 │──→│ 预测消息路由  │──→│ 消息转发到GameNode │      │
│  │ + 位压缩  │   └──────────────┘   └──────────────────┘      │
│  └──────────┘                                                 │
└───────────────────────────────────────────────────────────────┘
                              │
┌─────────────────────────────┼─────────────────────────────────┐
│                        GameNode                                 │
│  ┌──────────────────┐   ┌────▼───────┐   ┌─────────────────┐  │
│  │ 移动验证器        │←──│ 预测处理器  │──→│ 纠偏消息生成器   │  │
│  │ (权威物理模拟)    │   │            │   │ (差异计算+下发)  │  │
│  └────────┬─────────┘   └────────────┘   └─────────────────┘  │
│           │                                                     │
│  ┌────────▼─────────┐   ┌────────────┐   ┌─────────────────┐  │
│  │ 延迟补偿器        │   │ 快照管理器  │   │ 插值缓冲区       │  │
│  │ (服务器侧回溯)    │   │ (历史状态)  │   │ (其他玩家位置)   │  │
│  └──────────────────┘   └────────────┘   └─────────────────┘  │
└───────────────────────────────────────────────────────────────┘
```

### 1.3 预测流程

#### 1.3.1 客户端预测（本地执行）

```cpp
// [PRODUCTION] 客户端预测模拟器 — 本地物理执行，零延迟感知
class ClientPredictionSimulator {
public:
    // 玩家输入时立即本地执行预测
    void processInput(const PlayerInput& input) {
        // 1. 记录输入序列号
        uint32_t seq = m_next_seq++;
        m_pending_inputs.push({seq, input, m_predicted_state});

        // 2. 本地立即执行物理模拟（不等服务器）
        m_predicted_state = simulatePhysics(m_predicted_state, input, delta_time);

        // 3. 发送给服务器（UDP高频）
        sendToServer(seq, input);

        // 4. 渲染使用预测状态（零延迟）
        renderState = m_predicted_state;
    }

private:
    struct PendingInput {
        uint32_t seq;
        PlayerInput input;
        PhysicsState state_before;  // 执行前快照
    };

    uint32_t m_next_seq{0};
    std::deque<PendingInput> m_pending_inputs;  // 未确认的输入
    PhysicsState m_predicted_state;
};
```

#### 1.3.2 服务器验证（权威模拟）

```cpp
// [PRODUCTION] 服务器侧移动验证器 — 权威物理模拟
class ServerMovementValidator {
public:
    // 收到客户端输入后验证
    void processClientInput(uint64_t player_id, uint32_t seq, 
                            const PlayerInput& input) {
        Player& player = m_character_module.getPlayer(player_id);
        
        // 1. 权威物理模拟
        PhysicsState expected = simulatePhysics(player.state(), input, delta_time);
        
        // 2. 与客户端预测状态对比
        // 客户端同时发送了预测位置，服务器对比差异
        if (distance(expected.position, input.predicted_pos) > MAX_ERROR) {
            // 3a. 差异过大，发送纠偏
            sendReconciliation(player_id, seq, expected);
        } else {
            // 3b. 差异在容忍范围内，确认
            player.updateState(expected);
            sendConfirm(player_id, seq);
        }
        
        // 4. 记录快照（用于延迟补偿）
        m_snapshot_manager.record(player_id, seq, expected, now());
    }

private:
    static constexpr float MAX_ERROR = 0.5f;  // 0.5米误差容忍
};
```

#### 1.3.3 客户端纠偏

```cpp
// [PRODUCTION] 客户端纠偏器
class ClientReconciler {
public:
    void onServerState(uint32_t confirmed_seq, const PhysicsState& server_state) {
        // 1. 丢弃已确认的输入
        while (!m_pending_inputs.empty() && 
               m_pending_inputs.front().seq <= confirmed_seq) {
            m_pending_inputs.pop_front();
        }

        // 2. 检查是否需要纠偏
        if (distance(m_predicted_state.position, server_state.position) > MAX_ERROR) {
            // 3a. 回滚到服务器状态
            m_predicted_state = server_state;
            
            // 3b. 重放未确认的输入
            for (const auto& pending : m_pending_inputs) {
                m_predicted_state = simulatePhysics(m_predicted_state, 
                                                    pending.input, delta_time);
            }
            // 3c. 触发rubberband视觉效果（平滑过渡而非瞬移）
            triggerSmoothCorrection(server_state.position);
        }
        // 4. 差异在容忍范围内，不做处理（预测正确）
    }
};
```

### 1.4 延迟补偿（服务器侧回溯）

用于解决高延迟玩家"打不中"的问题：服务器在处理攻击命中判定时，回溯到攻击者发送命令时的世界状态。

```cpp
// [PRODUCTION] 延迟补偿器 — 服务器侧回溯命中判定
class LagCompensator {
public:
    // 玩家A发起攻击时调用
    bool checkHit(uint64_t attacker_id, uint64_t target_id, 
                  const Vector3& aim_point, uint32_t client_tick) {
        // 1. 估算攻击者的网络延迟
        int32_t latency_ms = m_net_module.getPing(attacker_id);
        
        // 2. 回溯到延迟前的世界状态
        int64_t rewind_time = now() - latency_ms - SAFETY_MARGIN_MS;
        auto snapshot = m_snapshot_manager.getSnapshotAt(target_id, rewind_time);
        
        if (!snapshot) {
            // 快照过期或不存在，拒绝命中
            return false;
        }
        
        // 3. 在回溯状态下做命中判定
        float dist = distance(aim_point, snapshot.position);
        return dist <= HIT_TOLERANCE;
    }

private:
    static constexpr int32_t SAFETY_MARGIN_MS = 30;  // 安全余量
    static constexpr float HIT_TOLERANCE = 1.0f;     // 命中容忍范围
};
```

### 1.5 其他玩家平滑插值

```cpp
// [PRODUCTION] 插值缓冲区 — 其他玩家移动平滑显示
class InterpolationBuffer {
public:
    // 收到其他玩家的位置更新
    void onPositionUpdate(uint64_t entity_id, const Vector3& pos, 
                          uint32_t server_tick) {
        m_buffers[entity_id].push({pos, server_tick, now()});
    }

    // 渲染时获取插值后的位置
    Vector3 getRenderPosition(uint64_t entity_id) {
        auto& buf = m_buffers[entity_id];
        
        // 渲染时间 = 服务器时间 - 插值延迟（100ms）
        int64_t render_time = serverNow() - INTERPOLATION_DELAY_MS;
        
        // 在两个快照之间线性插值
        auto& s1 = buf[buf.size() - 2];
        auto& s2 = buf[buf.size() - 1];
        
        float alpha = (float)(render_time - s1.recv_time) / 
                      (float)(s2.recv_time - s1.recv_time);
        return lerp(s1.position, s2.position, clamp(alpha, 0.0f, 1.0f));
    }

private:
    static constexpr int64_t INTERPOLATION_DELAY_MS = 100;  // 插值延迟
    struct Snapshot { Vector3 position; uint32_t tick; int64_t recv_time; };
    std::unordered_map<uint64_t, std::deque<Snapshot>> m_buffers;
};
```

### 1.6 协议定义

```
// FlatBuffers: 预测相关消息

table PlayerInputMessage {
    seq: uint32;           // 输入序列号
    input_flags: uint16;   // 按键位掩码
    delta_time: uint16;    // 帧间隔(ms)
    predicted_x: float;    // 预测位置X
    predicted_y: float;    // 预测位置Y
    predicted_z: float;    // 预测位置Z
    predicted_facing: ubyte; // 朝向(0-255映射0-360度)
}

table ReconciliationMessage {
    seq: uint32;           // 确认到的序列号
    correct_x: float;      // 服务器权威位置X
    correct_y: float;      // 服务器权威位置Y
    correct_z: float;      // 服务器权威位置Z
    correct_facing: ubyte; // 服务器权威朝向
    correction_type: ubyte; // 0=确认, 1=纠偏
}
```

### 1.7 参数调优建议

| 参数 | 推荐值 | 说明 |
|------|--------|------|
| MAX_ERROR | 0.5m | 预测误差容忍阈值，超过则纠偏 |
| INTERPOLATION_DELAY_MS | 100ms | 其他玩家插值延迟 |
| SAFETY_MARGIN_MS | 30ms | 延迟补偿安全余量 |
| HIT_TOLERANCE | 1.0m | 命中判定容忍范围 |
| 快照保留时长 | 1000ms | 用于延迟补偿回溯 |
| 纠偏平滑时间 | 200ms | rubberband视觉过渡时间 |

---

## 2. 寻路与碰撞系统 (P0)

### 2.1 设计目标

为GameNode提供高效的路径规划和碰撞检测能力，支持复杂地形导航、视线检查、动态碰撞 avoidance。

### 2.2 模块架构

```
game/navigation/           game/collision/
┌──────────────────┐      ┌──────────────────┐
│  NavMeshManager   │      │ CollisionSystem   │
│  ├─ 加载/卸载     │      │  ├─ 玩家碰撞      │
│  ├─ 路径规划      │      │  ├─ 怪物碰撞      │
│  └─ 路径平滑      │      │  ├─ 弹道碰撞      │
└────────┬─────────┘      │  └─ 地形碰撞      │
         │                 └────────┬─────────┘
         │                          │
         └──────────┬───────────────┘
                    │
          ┌─────────▼─────────┐
          │   LOSCacheManager  │
          │   ├─ 视线缓存表    │
          │   ├─ TTL自动过期   │
          │   └─ 空间哈希索引  │
          └───────────────────┘
```

### 2.3 NavMesh 寻路

#### 2.3.1 数据结构

```cpp
// [PRODUCTION] NavMesh导航网格管理器
class NavMeshManager {
public:
    // 按场景加载NavMesh数据
    void loadSceneNavMesh(uint32_t scene_id, const NavMeshData& data);
    void unloadSceneNavMesh(uint32_t scene_id);

    // A*路径规划
    PathResult findPath(uint32_t scene_id, 
                        const Vector3& start, 
                        const Vector3& end,
                        PathFindOptions opts = {});

    // 随机点生成（用于怪物巡逻）
    Vector3 getRandomReachablePoint(uint32_t scene_id, 
                                     const Vector3& center, 
                                     float radius);

    // 是否可达
    bool isReachable(uint32_t scene_id, 
                     const Vector3& from, 
                     const Vector3& to);

private:
    // 每个场景一个NavMesh实例
    std::unordered_map<uint32_t, std::unique_ptr<NavMesh>> m_navmeshes;
    
    // [EXPANSION_RISK] NavMesh内存占用大（单场景10-50MB）
    // 5万在线时需按需加载/卸载场景NavMesh
};

struct PathResult {
    std::vector<Vector3> waypoints;  // 路径点列表
    float total_distance;            // 总距离
    bool success;                    // 是否找到路径
};

struct PathFindOptions {
    bool smooth_path = true;        // 路径平滑（Funnel算法）
    float agent_radius = 0.3f;      // 寻路实体半径
    bool allow_off_mesh = false;    // 允许脱离网格（跳跃等）
    uint32_t max_nodes = 256;       // A*最大搜索节点数
};
```

#### 2.3.2 寻路算法选型

| 算法 | 用途 | 时间复杂度 | 说明 |
|------|------|-----------|------|
| A* on NavMesh | 标准寻路 | O(N log N) | N=多边形数量，场景通常<2000个 |
| Funnel Algorithm | 路径平滑 | O(N) | 消除Z字形路径，生成自然行走路径 |
| Dijkstra | 备选路径 | O(N²) | 当需要多条路径选择时使用 |
| string pulling | 路径简化 | O(N) | 减少路径点数量，降低后续处理开销 |

#### 2.3.3 性能优化

```cpp
// [PRODUCTION] 寻路请求队列 — 限制每帧寻路计算量
class PathfindScheduler {
public:
    // 提交寻路请求（异步）
    uint64_t requestPath(uint32_t scene_id, const Vector3& start, 
                         const Vector3& end, PathFindOptions opts) {
        uint64_t request_id = m_next_id++;
        m_pending_queue.push({request_id, scene_id, start, end, opts});
        return request_id;
    }

    // 每帧处理固定数量（防止卡帧）
    void tick() {
        uint32_t processed = 0;
        while (!m_pending_queue.empty() && processed < MAX_PATHFINDS_PER_TICK) {
            auto& req = m_pending_queue.front();
            PathResult result = m_navmesh.findPath(
                req.scene_id, req.start, req.end, req.opts);
            m_results[req.request_id] = result;
            // 通过事件总线通知请求者
            m_event_bus.publish(PathFoundEvent{req.request_id, result});
            m_pending_queue.pop();
            ++processed;
        }
    }

private:
    static constexpr uint32_t MAX_PATHFINDS_PER_TICK = 50;  // 每帧最多50次寻路
    // [EXPANSION_RISK] 1000人场景下寻路请求可能超过50/帧
    // 高峰期可提高到100，但需监控战斗循环耗时
};
```

### 2.4 碰撞检测系统

```cpp
// [PRODUCTION] 碰撞检测系统
class CollisionSystem {
public:
    // 实体注册（进入场景时调用）
    void registerEntity(uint64_t entity_id, const Collider& collider);
    void unregisterEntity(uint64_t entity_id);

    // 移动前碰撞检测
    bool checkMove(uint64_t entity_id, 
                   const Vector3& from, 
                   const Vector3& to,
                   CollisionResult& out_result);

    // 范围碰撞查询（AOE技能用）
    void queryRange(const Vector3& center, float radius,
                    std::vector<uint64_t>& out_entities);

    // 弹道碰撞检测（远程技能/箭矢）
    bool checkRaycast(const Vector3& origin, 
                      const Vector3& direction, 
                      float max_distance,
                      RaycastResult& out_result);

private:
    // 动态AABB树 — 高效空间查询
    DynamicAABBTree m_dynamic_tree;
    
    // 静态碰撞体（地形/建筑）— 独立管理
    std::unordered_map<uint32_t, StaticColliderData> m_static_colliders;
};

struct Collider {
    enum Type { SPHERE, CAPSULE, BOX } type;
    float radius;      // SPHERE/CAPSULE用
    Vector3 half_ext;  // BOX用
    float height;      // CAPSULE用
};

struct CollisionResult {
    bool collided;
    Vector3 hit_point;
    Vector3 hit_normal;
    uint64_t hit_entity_id;  // 0=地形碰撞
};

struct RaycastResult {
    bool hit;
    Vector3 hit_point;
    Vector3 hit_normal;
    uint64_t hit_entity_id;
    float distance;
};
```

### 2.5 LOS 视线缓存

这是战斗模块高频查询的关键优化：战斗循环中每次技能判定都需要检查施法者与目标之间是否有障碍物遮挡。

```cpp
// [PRODUCTION] LOS视线缓存 — 战斗循环零计算开销
class LOSCacheManager {
public:
    // 查询视线是否被阻挡（先查缓存）
    bool hasLineOfSight(uint32_t scene_id,
                        const Vector3& from, 
                        const Vector3& to) {
        // 1. 量化坐标到网格单元（减少缓存键数量）
        GridCell cell_from = quantize(from, GRID_SIZE);
        GridCell cell_to = quantize(to, GRID_SIZE);
        
        // 2. 查缓存
        CacheKey key{scene_id, cell_from, cell_to};
        auto it = m_cache.find(key);
        if (it != m_cache.end()) {
            m_hit_count++;
            return it->second;  // 缓存命中，直接返回
        }
        
        // 3. 缓存未命中，执行实际LOS计算
        bool result = m_collision.checkRaycast(
            from, (to - from).normalized(), 
            distance(from, to)) == false;
        
        // 4. 写入缓存
        m_cache[key] = result;
        m_miss_count++;
        return result;
    }

    // 定期清理过期缓存
    void pruneExpired() {
        auto now = steady_clock::now();
        for (auto it = m_cache.begin(); it != m_cache.end(); ) {
            if (now - it->second.timestamp > TTL) {
                it = m_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 缓存统计（监控用）
    float hitRate() const { 
        return (float)m_hit_count / (m_hit_count + m_miss_count); 
    }

private:
    static constexpr float GRID_SIZE = 2.0f;     // 2米网格量化
    static constexpr int64_t TTL = 5000;          // 5秒TTL
    
    struct GridCell { int32_t x, y, z; };
    struct CacheKey { uint32_t scene_id; GridCell from; GridCell to; };
    struct CacheEntry { bool has_los; int64_t timestamp; };
    
    struct KeyHash { /* custom hash */ };
    
    std::unordered_map<CacheKey, bool, KeyHash> m_cache;
    CollisionSystem& m_collision;
    std::atomic<uint64_t> m_hit_count{0};
    std::atomic<uint64_t> m_miss_count{0};
};
```

**性能预估**：
- 场景内网格单元数：1000m × 1000m / (2m × 2m) = 250,000 个
- 常用LOS对（战斗区域）：~5,000 对
- 缓存命中率：预估 >95%（同一区域战斗会重复查询相同网格对）
- 缓存命中耗时：~0.001ms（hash查找）
- 缓存未命中耗时：~0.1ms（raycast计算）

### 2.6 飞行路径系统

```cpp
// [PRODUCTION] 飞行路径系统 — 固定航线运输
class FlightPathSystem {
public:
    // 注册飞行航线
    void registerFlightPath(uint32_t path_id, 
                            const std::vector<Vector3>& waypoints,
                            float speed);

    // 玩家乘坐飞行坐骑
    uint64_t startFlight(uint64_t player_id, uint32_t path_id);
    
    // 取消飞行
    void cancelFlight(uint64_t player_id);

    // 每帧更新飞行中的玩家位置
    void tick(float delta_time);

private:
    struct FlightPath {
        uint32_t id;
        std::vector<Vector3> waypoints;
        float speed;
        float total_distance;
    };
    
    struct ActiveFlight {
        uint64_t player_id;
        uint32_t path_id;
        float progress;  // 0.0 ~ total_distance
    };
    
    std::unordered_map<uint32_t, FlightPath> m_paths;
    std::vector<ActiveFlight> m_active_flights;
};
```

---

## 3. AOI LOD 分级更新机制 (P0)

### 3.1 设计目标

在现有十字链表+动态网格算法基础上，引入LOD（Level of Detail）分级更新，按距离调整实体同步频率和属性精度，大幅降低带宽消耗。

### 3.2 LOD 分级策略

```
                    玩家位置
                        │
        ┌───────────────┼───────────────┐
        │               │               │
   LOD0 (近距)     LOD1 (中距)     LOD2 (远距)
   0 ~ 30m          30 ~ 80m        80 ~ 200m
   每帧更新(33ms)   每100ms更新     每500ms更新
   完整属性          关键属性         位置+朝向
```

```cpp
// [PRODUCTION] AOI LOD分级管理器
class AOILodManager {
public:
    enum class LODLevel {
        LOD0_NEAR,    // 0-30m:   每帧更新，完整属性
        LOD1_MID,     // 30-80m:  每100ms更新，关键属性
        LOD2_FAR,     // 80-200m: 每500ms更新，位置+朝向
        LOD3_OUT_OF_RANGE  // 200m+: 不更新
    };

    // 计算观察者到目标的LOD等级
    LODLevel calculateLOD(const Vector3& observer_pos, 
                          const Vector3& target_pos) {
        float dist_sq = distanceSquared(observer_pos, target_pos);
        
        if (dist_sq <= NEAR_DIST_SQ) return LODLevel::LOD0_NEAR;
        if (dist_sq <= MID_DIST_SQ) return LODLevel::LOD1_MID;
        if (dist_sq <= FAR_DIST_SQ) return LODLevel::LOD2_FAR;
        return LODLevel::LOD3_OUT_OF_RANGE;
    }

    // 判断是否需要发送更新
    bool shouldSendUpdate(LODLevel lod, int64_t last_send_time, 
                          int64_t now) {
        int64_t interval = getUpdateInterval(lod);
        return (now - last_send_time) >= interval;
    }

    // 根据LOD级别构建属性更新消息
    AttributeUpdate buildUpdate(uint64_t entity_id, LODLevel lod,
                                const PlayerCharacter& player) {
        switch (lod) {
            case LODLevel::LOD0_NEAR:
                return buildFullUpdate(player);  // 全属性
            case LODLevel::LOD1_MID:
                return buildKeyUpdate(player);   // 位置+朝向+HP+速度
            case LODLevel::LOD2_FAR:
                return buildMinimalUpdate(player); // 仅位置+朝向
            default:
                return AttributeUpdate{};  // 空更新
        }
    }

private:
    static constexpr float NEAR_DIST = 30.0f;
    static constexpr float MID_DIST = 80.0f;
    static constexpr float FAR_DIST = 200.0f;
    static constexpr float NEAR_DIST_SQ = NEAR_DIST * NEAR_DIST;
    static constexpr float MID_DIST_SQ = MID_DIST * MID_DIST;
    static constexpr float FAR_DIST_SQ = FAR_DIST * FAR_DIST;
    
    int64_t getUpdateInterval(LODLevel lod) {
        switch (lod) {
            case LODLevel::LOD0_NEAR: return 33;   // ~30fps
            case LODLevel::LOD1_MID:  return 100;  // 10fps
            case LODLevel::LOD2_FAR:  return 500;  // 2fps
            default: return INT64_MAX;
        }
    }
};
```

### 3.3 属性压缩

```cpp
// [PRODUCTION] LOD属性压缩 — 按距离裁剪属性集

// LOD0: 完整属性（近距战斗需要全部信息）
struct FullAttributeUpdate {
    Vector3 position;        // 12 bytes
    Vector3 velocity;        // 12 bytes
    uint8_t facing;          // 1 byte (0-255 -> 0-360度)
    int32_t hp;              // 4 bytes
    int32_t max_hp;          // 4 bytes
    int32_t mp;              // 4 bytes
    int32_t max_mp;          // 4 bytes
    uint16_t animation;      // 2 bytes (动画状态)
    uint16_t buff_flags;     // 2 bytes (可见Buff位掩码)
    uint8_t combat_state;    // 1 byte (战斗/和平/施法)
    uint8_t movement_mode;   // 1 byte (行走/跑步/游泳/飞行)
    // 总计: ~47 bytes
};

// LOD1: 关键属性（中距只需判断是否需要避让/追击）
struct KeyAttributeUpdate {
    int16_t pos_x, pos_y, pos_z;  // 6 bytes (量化到0.1m精度)
    uint8_t facing;                // 1 byte
    int16_t hp;                    // 2 bytes (百分比0-1000)
    uint8_t movement_mode;         // 1 byte
    // 总计: ~10 bytes
};

// LOD2: 最小属性（远距只需知道大概位置）
struct MinimalAttributeUpdate {
    int16_t pos_x, pos_z;     // 4 bytes (量化到1m精度, 省略Y轴)
    uint8_t facing;            // 1 byte
    // 总计: ~5 bytes
};
```

**带宽节省预估**（1000人场景，平均分布）：

| LOD级别 | 人数占比 | 人数 | 每人带宽/帧 | 小计 |
|---------|---------|------|-----------|------|
| LOD0 (近距) | 5% | 50 | 47B × 30fps = 11.1KB/s | 555KB/s |
| LOD1 (中距) | 20% | 200 | 10B × 10fps = 0.8KB/s | 160KB/s |
| LOD2 (远距) | 75% | 750 | 5B × 2fps = 0.08KB/s | 60KB/s |
| **总计** | 100% | 1000 | - | **775KB/s** |

对比优化前（全部每帧发送47B）：
- 优化前: 1000 × 47B × 30fps = **1,365KB/s**
- 优化后: **775KB/s**
- **节省: 43%**

加上增量编码（见第6节位压缩），可进一步降低到 ~300KB/s。

### 3.4 视野锥裁剪

```cpp
// [PRODUCTION] 视野锥裁剪 — 基于朝向减少不必要同步
class ViewConeCuller {
public:
    // 判断目标是否在观察者视野锥内
    bool isInViewCone(const Vector3& observer_pos,
                      uint8_t observer_facing,
                      const Vector3& target_pos) {
        Vector3 to_target = target_pos - observer_pos;
        float dist = to_target.length();
        if (dist > VIEW_RANGE) return false;
        
        // 观察者朝向向量
        float facing_rad = (float)observer_facing / 255.0f * 2.0f * PI;
        Vector3 facing_dir(sinf(facing_rad), 0, cosf(facing_rad));
        
        // 计算夹角
        float cos_angle = dot(facing_dir, to_target.normalized());
        float angle = acosf(cos_angle);
        
        return angle <= HALF_CONE_ANGLE;  // 视野锥半角
    }

private:
    static constexpr float VIEW_RANGE = 200.0f;       // 最大视野距离
    static constexpr float HALF_CONE_ANGLE = PI * 0.75f; // 270度视野
    // 注意: MMO通常用270度而非90度视野锥，因为玩家可以旋转视角
};
```

### 3.5 动态负载均衡

```cpp
// [PRODUCTION] AOI动态频率调整 — 拥挤区域自动降频
class DynamicLodBalancer {
public:
    // 每个网格区域统计实体密度
    void updateDensity(uint32_t scene_id, 
                       const std::unordered_map<GridCell, uint32_t>& densities) {
        for (const auto& [cell, count] : densities) {
            if (count > CROWDED_THRESHOLD) {
                // 拥挤区域：LOD0范围缩小，LOD1/LOD2范围扩大
                m_lod_params.near_dist = 15.0f;  // 30m -> 15m
                m_lod_params.mid_dist = 50.0f;   // 80m -> 50m
            } else {
                // 正常区域：恢复标准参数
                m_lod_params.near_dist = 30.0f;
                m_lod_params.mid_dist = 80.0f;
            }
        }
    }

private:
    static constexpr uint32_t CROWDED_THRESHOLD = 100;  // 100人/网格 = 拥挤
    struct LodParams { float near_dist, mid_dist, far_dist; };
    LodParams m_lod_params{30.0f, 80.0f, 200.0f};
};
```

### 3.6 增强后的AOI模块接口

```cpp
// [PRODUCTION] 增强版AOI管理器
class AOIManager {
public:
    // === 原有接口 ===
    void onEntityEnter(uint64_t entity_id, const Vector3& pos);
    void onEntityLeave(uint64_t entity_id);
    void onEntityMove(uint64_t entity_id, const Vector3& new_pos);
    
    // === 新增: LOD感知的视野更新 ===
    void tick(int64_t now) {
        for (auto& [observer_id, observer] : m_observers) {
            // 1. 获取观察者周围的所有实体
            auto entities = m_grid.queryRange(observer.position, VIEW_RANGE);
            
            for (uint64_t target_id : entities) {
                if (target_id == observer_id) continue;
                
                auto& target = m_entities[target_id];
                
                // 2. 视野锥裁剪
                if (!m_culler.isInViewCone(observer.position, 
                                           observer.facing, target.position))
                    continue;
                
                // 3. 计算LOD级别
                LODLevel lod = m_lod_manager.calculateLOD(
                    observer.position, target.position);
                
                if (lod == LODLevel::LOD3_OUT_OF_RANGE) continue;
                
                // 4. 检查是否到了更新时间
                auto& last_send = m_last_send[observer_id][target_id];
                if (!m_lod_manager.shouldSendUpdate(lod, last_send, now))
                    continue;
                
                // 5. 构建并发送LOD级别的更新
                auto update = m_lod_manager.buildUpdate(
                    target_id, lod, *target.player);
                m_event_bus.publish(AOIUpdateEvent{
                    observer_id, target_id, update, lod});
                
                last_send = now;
            }
        }
    }

private:
    DynamicGrid m_grid;                    // 动态网格（原有）
    CrossLinkedList m_cross_list;          // 十字链表（原有）
    AOILodManager m_lod_manager;           // LOD分级（新增）
    ViewConeCuller m_culler;               // 视野锥裁剪（新增）
    DynamicLodBalancer m_balancer;         // 动态负载均衡（新增）
    
    struct ObserverInfo { Vector3 position; uint8_t facing; };
    std::unordered_map<uint64_t, ObserverInfo> m_observers;
    std::unordered_map<uint64_t, EntityInfo> m_entities;
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, int64_t>> m_last_send;
};
```

---

## 4. 场景 Phasing/Layering 系统 (P1)

### 4.1 设计目标

引入Phasing（相位）和Layering（分层）技术，实现同一物理坐标下不同玩家看到不同世界状态的能力。

### 4.2 概念定义

| 概念 | 定义 | 触发条件 | 典型场景 |
|------|------|---------|---------|
| Phase（相位） | 基于玩家进度展示不同的世界状态 | 任务完成/剧情进度 | NPC出现/消失、建筑变化、地形改变 |
| Layer（分层） | 同一服务器的平行副本，自动均衡玩家 | 区域人数超阈值 | 主城人太多时拆分为多个层 |
| Instance（副本） | 独立进程隔离的私人空间 | 队伍进入副本入口 | 5人本/团本 |
| CrossServerZone（跨服区） | 多个集群节点共享的区域 | 低人口区域合并 | 跨服战场、跨服野外Boss |

### 4.3 Phase 系统

```cpp
// [PRODUCTION] 场景相位管理器
class PhaseManager {
public:
    // 玩家进入场景时确定相位
    uint32_t resolvePhase(uint64_t player_id, uint32_t scene_id) {
        // 1. 收集玩家的相位条件
        auto conditions = m_quest_module.getPhaseConditions(player_id, scene_id);
        
        // 2. 匹配相位规则
        for (const auto& rule : m_phase_rules[scene_id]) {
            if (rule.matches(conditions)) {
                return rule.phase_id;
            }
        }
        
        // 3. 默认相位
        return DEFAULT_PHASE;
    }

    // 注册相位规则
    void registerPhaseRule(uint32_t scene_id, const PhaseRule& rule);

    // 玩家相位切换（任务完成触发）
    void onPhaseChange(uint64_t player_id, uint32_t old_phase, uint32_t new_phase) {
        // 1. 从旧相位的AOI中移除
        m_aoi_manager.onEntityLeave(player_id);
        
        // 2. 更新相位
        m_player_phases[player_id] = new_phase;
        
        // 3. 加入新相位的AOI
        m_aoi_manager.onEntityEnter(player_id, getPlayerPosition(player_id));
        
        // 4. 通知客户端场景变化
        m_event_bus.publish(PhaseChangedEvent{
            player_id, old_phase, new_phase});
    }

private:
    static constexpr uint32_t DEFAULT_PHASE = 0;
    
    struct PhaseRule {
        uint32_t phase_id;
        std::vector<PhaseCondition> conditions;
        bool matches(const std::vector<PhaseCondition>& player_conds) const;
    };
    
    std::unordered_map<uint32_t, std::vector<PhaseRule>> m_phase_rules;
    std::unordered_map<uint64_t, uint32_t> m_player_phases;
};
```

### 4.4 Layer 系统

```cpp
// [PRODUCTION] 场景分层管理器
class LayerManager {
public:
    // 玩家进入场景时分配层
    uint32_t assignLayer(uint32_t scene_id, uint64_t player_id) {
        auto& layers = m_scene_layers[scene_id];
        
        // 1. 查找人数最少的层
        uint32_t min_count = UINT32_MAX;
        uint32_t best_layer = 0;
        
        for (auto& [layer_id, layer] : layers) {
            if (layer.player_count < layer.capacity && 
                layer.player_count < min_count) {
                min_count = layer.player_count;
                best_layer = layer_id;
            }
        }
        
        // 2. 所有层都满了，创建新层
        if (min_count == UINT32_MAX) {
            best_layer = createNewLayer(scene_id);
        }
        
        // 3. 分配到选中的层
        layers[best_layer].player_ids.insert(player_id);
        layers[best_layer].player_count++;
        
        return best_layer;
    }

    // 玩家离开场景时释放层
    void releaseLayer(uint32_t scene_id, uint32_t layer_id, uint64_t player_id) {
        auto& layers = m_scene_layers[scene_id];
        auto& layer = layers[layer_id];
        layer.player_ids.erase(player_id);
        layer.player_count--;
        
        // 层空了则回收
        if (layer.player_count == 0 && layers.size() > 1) {
            layers.erase(layer_id);
        }
    }

    // 同层玩家才能互相可见
    bool canSeeEachOther(uint64_t player_a, uint64_t player_b) {
        auto it_a = m_player_layer.find(player_a);
        auto it_b = m_player_layer.find(player_b);
        if (it_a == m_player_layer.end() || it_b == m_player_layer.end())
            return false;
        return it_a->second == it_b->second;
    }

private:
    static constexpr uint32_t LAYER_CAPACITY = 1200;  // 单层容量（野外场景）
    
    struct SceneLayer {
        uint32_t id;
        std::unordered_set<uint64_t> player_ids;
        uint32_t player_count;
        uint32_t capacity;
    };
    
    std::unordered_map<uint32_t, std::map<uint32_t, SceneLayer>> m_scene_layers;
    std::unordered_map<uint64_t, uint32_t> m_player_layer;
};
```

### 4.5 跨服区域

```cpp
// [PROTOTYPE] 跨服区域管理器 — 基于Redis Pub/Sub
class CrossServerZone {
public:
    // 注册跨服区域
    void registerCrossZone(uint32_t zone_id, 
                           const std::vector<std::string>& node_ids);

    // 玩家进入跨服区域
    void onPlayerEnterCrossZone(uint64_t player_id, uint32_t zone_id) {
        // 1. 通知所有相关节点
        m_redis_pubsub.publish(
            "cross_zone:" + std::to_string(zone_id),
            serialize(PlayerEnterCrossZone{player_id, zone_id, m_local_node_id}));
    }

    // 接收其他节点的玩家进入通知
    void onRemotePlayerEnter(const PlayerEnterCrossZone& msg) {
        // 在本地AOI中注册远程玩家（影子实体）
        m_aoi_manager.onRemoteEntityEnter(msg.player_id, msg.node_id);
    }

private:
    // Redis Pub/Sub 频道: "cross_zone:{zone_id}"
    // 消息类型: PlayerEnterCrossZone / PlayerMoveCrossZone / PlayerLeaveCrossZone
    RedisPubSub m_redis_pubsub;
    std::string m_local_node_id;
};
```

### 4.6 场景调度增强

```cpp
// [PRODUCTION] 增强版场景调度器
class SceneScheduler {
public:
    // 场景类型决定调度策略
    enum class SceneType {
        OPEN_WORLD,    // 开放世界：Phase + Layer
        DUNGEON,       // 副本：独立进程隔离
        BATTLEGROUND,  // 战场：跨服区域
        CITY,          // 主城：Layer（高容量）
    };

    void onPlayerEnterScene(uint64_t player_id, uint32_t scene_id, 
                            SceneType type) {
        switch (type) {
            case SceneType::OPEN_WORLD: {
                uint32_t phase = m_phase_manager.resolvePhase(player_id, scene_id);
                uint32_t layer = m_layer_manager.assignLayer(scene_id, player_id);
                // AOI使用 phase + layer 组合作为可见性过滤器
                m_aoi_manager.setPlayerFilter(player_id, phase, layer);
                break;
            }
            case SceneType::DUNGEON: {
                // 分配独立GameNode进程
                uint32_t instance_id = createDungeonInstance(scene_id, player_id);
                m_router.routeToInstance(player_id, instance_id);
                break;
            }
            case SceneType::BATTLEGROUND: {
                // 跨服匹配
                m_cross_server.matchBattleground(player_id);
                break;
            }
            case SceneType::CITY: {
                uint32_t layer = m_layer_manager.assignLayer(scene_id, player_id);
                m_aoi_manager.setPlayerFilter(player_id, DEFAULT_PHASE, layer);
                break;
            }
        }
    }

private:
    PhaseManager m_phase_manager;
    LayerManager m_layer_manager;
    CrossServerZone m_cross_server;
    AOIManager& m_aoi_manager;
};
```

---

## 5. 法术批次处理与 LOS 缓存 (P1)

### 5.1 Spell Batching（法术批次处理）

#### 5.1.1 设计原理

WoW早期版本的Spell Batching将400ms内的法术同时处理，保证公平性——两个玩家同时对轰火球时，两个火球同时造成伤害，而非先后结算。这解决了"谁先手谁赢"的网络延迟不公平问题。

#### 5.1.2 批次窗口设计

```cpp
// [PRODUCTION] 法术批次处理器
class SpellBatchProcessor {
public:
    // 提交施法请求
    void submitSpellCast(uint64_t caster_id, uint32_t spell_id,
                         uint64_t target_id, int64_t submit_time) {
        m_pending_spells.push({
            caster_id, spell_id, target_id, submit_time
        });
    }

    // 每批次窗口结束时统一处理
    void processBatch() {
        int64_t now = steady_clock::now();
        
        // 1. 收集批次窗口内的所有法术
        std::vector<SpellCast> batch;
        while (!m_pending_spells.empty() && 
               m_pending_spells.front().submit_time <= now) {
            batch.push_back(m_pending_spells.front());
            m_pending_spells.pop();
        }
        
        if (batch.empty()) return;
        
        // 2. 冲突检测与排序
        // 同一目标被多个法术命中时，按特定规则排序：
        //   - 驱散 > 控制 > 伤害（驱散优先，可能改变后续结算）
        //   - 同优先级按施法者ID排序（确定性，避免随机）
        std::sort(batch.begin(), batch.end(), [](const SpellCast& a, const SpellCast& b) {
            int pri_a = getSpellPriority(a.spell_id);
            int pri_b = getSpellPriority(b.spell_id);
            if (pri_a != pri_b) return pri_a > pri_b;
            return a.caster_id < b.caster_id;
        });
        
        // 3. 统一快照 — 所有法术基于同一时刻的状态结算
        auto snapshot = captureWorldSnapshot();
        
        // 4. 逐个结算（基于快照，互不影响）
        std::vector<SpellResult> results;
        for (const auto& cast : batch) {
            // 检查施法者是否还活着（快照时刻）
            if (!snapshot.isAlive(cast.caster_id)) continue;
            
            // 检查目标是否还活着
            if (!snapshot.isAlive(cast.target_id)) {
                // 目标已死，法术失败
                results.push_back({cast, SpellResultCode::TARGET_DEAD});
                continue;
            }
            
            // 结算法术效果
            auto result = m_combbat_system.resolveSpell(cast, snapshot);
            results.push_back({cast, result});
        }
        
        // 5. 应用所有结果（同时生效）
        for (const auto& [cast, result] : results) {
            applySpellResult(cast, result);
        }
        
        // 6. 通过事件总线发布批次结果
        m_event_bus.publish(SpellBatchEvent{results});
    }

private:
    static constexpr int64_t BATCH_WINDOW_MS = 100;  // 批次窗口（100ms）
    
    // [EXPANSION_RISK] 批次窗口影响操作响应感
    // 100ms是平衡点：太短失去公平性，太长感觉延迟
    // PVP场景可用200ms，PVE场景可关闭batching
    
    struct SpellCast {
        uint64_t caster_id;
        uint32_t spell_id;
        uint64_t target_id;
        int64_t submit_time;
    };
    
    struct SpellResult {
        SpellCast cast;
        SpellResultCode code;
    };
    
    std::queue<SpellCast> m_pending_spells;
};
```

#### 5.1.3 批次窗口选择

| 场景 | 批次窗口 | 理由 |
|------|---------|------|
| PVP（竞技场/战场） | 200ms | 公平性优先，保证双方同时结算 |
| PVP（野外） | 100ms | 平衡公平性和响应感 |
| PVE | 0ms（关闭） | 不需要批次，即时结算 |
| 混合（野外PVP+PVE） | 100ms | 默认值，兼顾两者 |

### 5.2 LOS 缓存与战斗模块集成

```cpp
// [PRODUCTION] 战斗模块增强 — LOS缓存集成
class CombatSystem {
public:
    // 技能释放时检查视线
    bool canCastSpell(uint64_t caster_id, uint64_t target_id, 
                      uint32_t spell_id) {
        const auto& caster = m_character_module.getPlayer(caster_id);
        const auto& target = m_character_module.getPlayer(target_id);
        
        // 1. 距离检查
        float dist = distance(caster.position(), target.position());
        if (dist > getSpellRange(spell_id)) return false;
        
        // 2. LOS检查（走缓存，零开销）
        if (requiresLOS(spell_id)) {
            if (!m_los_cache.hasLineOfSight(
                    caster.scene_id(), 
                    caster.position(), 
                    target.position())) {
                return false;  // 视线被阻挡
            }
        }
        
        // 3. 其他条件检查（冷却/资源/姿态等）
        return checkSpellConditions(caster, spell_id);
    }

    // AOE伤害结算时批量LOS检查
    void resolveAOE(uint64_t caster_id, const Vector3& center, 
                    float radius, int32_t damage) {
        // 1. 范围碰撞查询获取所有受影响实体
        std::vector<uint64_t> targets;
        m_collision.queryRange(center, radius, targets);
        
        // 2. 批量LOS检查（缓存命中率高）
        for (uint64_t target_id : targets) {
            const auto& target = m_character_module.getPlayer(target_id);
            
            // LOS缓存查询 — 大部分命中缓存，O(1)
            if (m_los_cache.hasLineOfSight(
                    target.scene_id(), center, target.position())) {
                // 视线无遮挡，施加伤害
                target.applyDamage(caster_id, damage, DamageType::AOE);
            }
        }
    }

private:
    LOSCacheManager m_los_cache;       // LOS缓存（新增）
    CollisionSystem m_collision;       // 碰撞系统（新增）
    CharacterModule& m_character_module;
    EventBus& m_event_bus;
};
```

---

## 6. 高频消息位压缩方案 (P1)

### 6.1 设计目标

为30fps位置更新等高频消息设计专用位压缩编码，在保留FlatBuffers用于复杂消息的同时，将高频消息带宽降低60%以上。

### 6.2 双轨协议策略

```
┌──────────────────────────────────────────────────────────┐
│                   消息类型路由                             │
│                                                          │
│  高频消息 (<50 bytes, 30fps)    复杂消息 (>50 bytes)      │
│  ┌────────────────────┐        ┌────────────────────┐    │
│  │ 自定义位压缩编码    │        │ FlatBuffers        │    │
│  │ - 位置更新          │        │ - 登录/角色信息     │    │
│  │ - 技能释放          │        │ - 背包操作          │    │
│  │ - AOI视野更新       │        │ - 交易/拍卖         │    │
│  │ - 移动输入          │        │ - 任务/社交         │    │
│  └────────────────────┘        └────────────────────┘    │
│  带宽: ~5-15 bytes/消息         带宽: ~100-500 bytes/消息 │
│  压缩率: 60-80%                 压缩率: 0% (零拷贝优势)   │
└──────────────────────────────────────────────────────────┘
```

### 6.3 位压缩编码方案

```cpp
// [PRODUCTION] 高频消息位压缩编解码器
class HighFreqCodec {
public:
    // === 位置更新消息编码 ===
    // 增量编码: 只发送与上次位置的差值
    BitBuffer encodePositionUpdate(uint64_t entity_id,
                                    const Vector3& new_pos,
                                    uint8_t facing,
                                    const Vector3& last_pos) {
        BitBuffer buf;
        
        // 1. 消息类型 (2 bits)
        buf.writeBits(MSG_POSITION_UPDATE, 2);
        
        // 2. 增量位置 (变长编码)
        Vector3 delta = new_pos - last_pos;
        encodeDelta(buf, delta.x);
        encodeDelta(buf, delta.y);
        encodeDelta(buf, delta.z);
        
        // 3. 朝向 (8 bits, 0-255映射0-360度)
        buf.writeBits(facing, 8);
        
        return buf;
    }
    
    // 增量编码: 小位移用少bit，大位移用多bit
    void encodeDelta(BitBuffer& buf, float delta) {
        int32_t quantized = (int32_t)(delta * 10.0f);  // 0.1m精度
        
        if (quantized == 0) {
            buf.writeBits(0, 1);  // 1 bit: 无变化
        } else if (quantized >= -3 && quantized <= 3) {
            buf.writeBits(1, 1);  // 标记: 3-bit范围
            buf.writeBits(quantized + 3, 3);  // 3 bits: -3~+3
        } else if (quantized >= -31 && quantized <= 31) {
            buf.writeBits(2, 2);  // 标记: 5-bit范围
            buf.writeBits(quantized + 31, 6);  // 6 bits: -31~+31
        } else if (quantized >= -511 && quantized <= 511) {
            buf.writeBits(3, 2);  // 标记: 9-bit范围
            buf.writeBits(quantized + 511, 10);  // 10 bits: -511~+511
        } else {
            buf.writeBits(4, 3);  // 标记: 全精度
            buf.writeBits(quantized, 32);  // 32 bits: 完整int32
        }
    }

    // === 技能释放消息编码 ===
    BitBuffer encodeSpellCast(uint64_t caster_id, uint32_t spell_id,
                               uint64_t target_id, uint8_t facing) {
        BitBuffer buf;
        
        buf.writeBits(MSG_SPELL_CAST, 2);
        buf.writeBits(caster_id & 0xFFFF, 16);  // 低16位（场景内ID）
        buf.writeBits(spell_id & 0xFFF, 12);     // 12-bit法术ID（4096种）
        buf.writeBits(target_id & 0xFFFF, 16);   // 目标ID
        buf.writeBits(facing, 8);                 // 朝向
        
        return buf;
    }

private:
    static constexpr uint8_t MSG_POSITION_UPDATE = 0;
    static constexpr uint8_t MSG_SPELL_CAST = 1;
    static constexpr uint8_t MSG_AOI_UPDATE = 2;
    static constexpr uint8_t MSG_MOVE_INPUT = 3;
};

// [PRODUCTION] 位缓冲区 — 位级读写
class BitBuffer {
public:
    void writeBits(uint32_t value, uint8_t bit_count);
    uint32_t readBits(uint8_t bit_count);
    
    const uint8_t* data() const { return m_buffer.data(); }
    size_t byteSize() const { return (m_bit_pos + 7) / 8; }
    
private:
    std::vector<uint8_t> m_buffer;
    size_t m_bit_pos{0};
};
```

### 6.4 带宽对比

| 消息类型 | FlatBuffers大小 | 位压缩大小 | 节省 |
|---------|----------------|-----------|------|
| 位置更新(增量小) | 48 bytes | 4 bytes | 92% |
| 位置更新(增量中) | 48 bytes | 10 bytes | 79% |
| 位置更新(增量大) | 48 bytes | 16 bytes | 67% |
| 技能释放 | 56 bytes | 7 bytes | 88% |
| AOI视野更新(LOD0) | 52 bytes | 20 bytes | 62% |
| AOI视野更新(LOD2) | 24 bytes | 5 bytes | 79% |

**1000人场景带宽估算**（结合LOD分级）：

| 消息类型 | 频率 | 每条大小 | 数量 | 带宽 |
|---------|------|---------|------|------|
| LOD0位置更新 | 30fps | 8B(平均) | 50人 | 12KB/s |
| LOD1位置更新 | 10fps | 12B | 200人 | 24KB/s |
| LOD2位置更新 | 2fps | 5B | 750人 | 7.5KB/s |
| 技能/战斗 | 5fps | 7B | 100人 | 3.5KB/s |
| **总计** | - | - | - | **47KB/s** |

对比优化前（FlatBuffers全部LOD0）：**1,365KB/s → 47KB/s，节省97%**

### 6.5 协议头设计

```
┌─────────┬───────────┬─────────────────┬──────────────────┐
│ 长度     │ 消息类型   │ 消息体           │ CRC校验           │
│ 2 bytes │ 1 byte    │ 变长             │ 2 bytes          │
│         │ 0=FlatBuf │ 高频:位压缩      │ (可选,UDP必选)    │
│         │ 1=BitPack │ 低频:FlatBuffers │                   │
│         │ 2=Raw     │                  │                   │
└─────────┴───────────┴─────────────────┴──────────────────┘
```

---

## 7. 架构变更总览

### 7.1 新增ADR

#### ADR-005: 客户端预测与服务器纠偏

**决策**: 采用客户端预测+服务器权威验证+纠偏的架构，客户端本地执行物理模拟，服务器验证并纠偏。

**理由**: 无预测的MMO在网络延迟下体验极差（每次操作需等待RTT才生效）。预测让操作"零延迟"感知，服务器纠偏保证数据一致性。延迟补偿解决高延迟玩家的命中判定问题。

**代价**: 客户端需要实现物理模拟逻辑（增加客户端复杂度），服务器需要维护历史快照（内存开销）。

#### ADR-006: NavMesh寻路 + LOS缓存

**决策**: 使用NavMesh作为寻路基础设施，LOS检查结果缓存到空间哈希表（2米网格量化，5秒TTL）。

**理由**: NavMesh是3D空间寻路的事实标准。LOS缓存将战斗循环中的视线检查从O(N)计算降到O(1)查表，缓存命中率预估>95%。

**代价**: NavMesh数据需要预生成和按场景加载（每场景10-50MB内存），LOS缓存需要定期清理（占用额外内存）。

#### ADR-007: AOI LOD三级更新

**决策**: 按距离分LOD0(30m/每帧)/LOD1(80m/100ms)/LOD2(200m/500ms)三级更新频率，配合属性压缩和视野锥裁剪。

**理由**: 全量每帧更新1000人消耗1.3MB/s带宽。LOD分级+属性压缩可将带宽降至47KB/s，节省97%。

**代价**: 远距离玩家位置更新有500ms延迟（视觉上可接受），拥挤区域需要动态调整LOD参数。

#### ADR-008: 法术批次处理

**决策**: PVP场景启用100-200ms法术批次窗口，PVE场景关闭批次即时结算。

**理由**: 批次处理保证PVP公平性——双方同时对轰时同时结算伤害，而非先后。WoW早期400ms窗口过于保守，100ms在公平性和响应感之间取得平衡。

**代价**: 法术结算有100-200ms延迟（PVP场景），需要玩家可接受。

#### ADR-009: 高频消息双轨协议

**决策**: 高频消息（位置/技能/AOI）使用自定义位压缩编码，复杂消息保留FlatBuffers。

**理由**: FlatBuffers虽然零拷贝反序列化，但消息体比专用协议大2-5倍。位压缩可将1000人场景带宽从1.3MB/s降至47KB/s。复杂消息保留FlatBuffers的schema演进能力。

**代价**: 需要维护两套编解码器，位压缩协议的schema演进需要手动处理。

#### ADR-010: 场景Phasing/Layering

**决策**: 引入Phase（任务进度驱动的世界状态分层）和Layer（平行层自动均衡）两种机制。

**理由**: Phase支持基于进度的动态世界变化（WoW核心特性），Layer解决热门区域人数过载问题。两者结合实现灵活的场景管理。

**代价**: AOI需要同时过滤Phase和Layer，增加查询复杂度。跨Layer/Phase交互需要特殊处理。

### 7.2 更新后的通信矩阵

| 通信场景 | 方式 | 延迟目标 | 变更说明 |
|---------|------|---------|---------|
| GameNode 内模块间 | 进程内 EventBus | <0.01ms | 无变化 |
| Gateway ↔ GameNode | gRPC (内部) | <1ms | 无变化 |
| GameNode ↔ DataService | gRPC | <2ms | 无变化 |
| 客户端 ↔ Gateway (高频) | UDP + 位压缩 | - | **新增: 位压缩编码** |
| 客户端 ↔ Gateway (可靠) | TCP + FlatBuffers | - | 无变化 |
| 寻路请求 | GameNode内部 | <0.5ms | **新增: NavMesh** |
| LOS查询 | GameNode内部 | <0.001ms | **新增: 缓存命中** |
| 法术批次 | GameNode内部 | 100-200ms | **新增: 批次窗口** |

### 7.3 更新后的模块清单

| 层 | 原有模块 | 新增模块 | 增强模块 |
|----|---------|---------|---------|
| Gateway | 连接/编解码/安全/路由/迁移 | - | 编解码(+位压缩) |
| GameNode | 场景/角色/战斗/AOI/社交/任务/经济 | **预测/寻路/碰撞** | AOI(+LOD)/战斗(+批次+LOS)/场景(+Phase+Layer) |
| DataService | Redis代理/MySQL代理/同步/版本 | - | - |
| Common | 定时器/日志/监控/排行榜/防外挂 | - | - |
| Ops | 编排/热更/灰度/配置/自愈 | - | - |

### 7.4 更新后的性能红线

| 指标 | 原红线 | 新红线 | 变更说明 |
|------|--------|--------|---------|
| 战斗循环耗时(1000人) | <=5ms | <=5ms | 不变（LOS缓存确保） |
| 单节点消息吞吐 | <=8000条/s | <=8000条/s | 不变 |
| 连接迁移时间 | <=800ms | <=800ms | 不变 |
| 高频消息带宽(1000人) | ~1.3MB/s | **<=50KB/s** | **新增红线** |
| NavMesh寻路延迟 | - | **<=0.5ms** | **新增红线** |
| LOS缓存命中率 | - | **>=90%** | **新增红线** |
| 预测纠偏频率 | - | **<=5%** | **新增红线**（95%预测正确） |

---

## 8. 实施计划

### 8.1 实施顺序

```
Phase 1 (Week 2): P0 基础设施
  ├─ Day 1: 创建新模块目录 + 接口定义
  ├─ Day 2: NavMesh寻路 + 碰撞系统骨架
  ├─ Day 3: LOS缓存 + 客户端预测协议
  ├─ Day 4: AOI LOD分级实现
  └─ Day 5: 集成测试 + 性能验证

Phase 2 (Week 3): P1 增强功能
  ├─ Day 1-2: 法术批次处理
  ├─ Day 3-4: 场景Phasing/Layering
  └─ Day 5: 高频消息位压缩编解码器

Phase 3 (Week 4): 集成与验证
  ├─ Day 1-2: 全模块集成测试
  ├─ Day 3-4: 1000人压测验证
  └─ Day 5: 性能调优 + 文档更新
```

### 8.2 验收标准

| # | 验收项 | 验收标准 |
|---|--------|---------|
| 1 | 客户端预测 | 95%以上预测正确，纠偏频率<=5% |
| 2 | 寻路系统 | 单次寻路<=0.5ms，路径有效率>=99% |
| 3 | LOS缓存 | 命中率>=90%，缓存查询<=0.001ms |
| 4 | AOI LOD | 1000人带宽<=50KB/s，远距玩家视觉无卡顿 |
| 5 | 法术批次 | PVP场景双方同时结算，延迟<=200ms |
| 6 | 位压缩 | 高频消息压缩率>=60%，编解码<=0.01ms |
| 7 | 场景分层 | 主城Layer自动均衡，Phase切换<=500ms |

---

## 附录: 与WoW技术对标更新

| 维度 | 优化前 | 优化后 | WoW水平 |
|------|--------|--------|---------|
| 客户端预测 | 缺失 | 预测+纠偏+延迟补偿+插值 | 成熟方案，基本对齐 |
| 寻路碰撞 | 缺失 | NavMesh+AABB树+LOS缓存 | NavMesh+LOS缓存，基本对齐 |
| AOI优化 | 十字链表+网格 | +LOD三级+属性压缩+视野锥+动态均衡 | LOD+压缩+视野锥，基本对齐 |
| 场景分层 | 无 | Phase+Layer+跨服区域 | Phasing+Layering+跨服，基本对齐 |
| 法术系统 | 框架 | +批次处理+LOS集成 | 批次处理+完整公式，架构对齐 |
| 协议紧凑度 | FlatBuffers | 双轨: 位压缩+FlatBuffers | 专用协议，接近对齐 |
| 战斗系统深度 | 框架 | 框架+批次+LOS | 20年积累，仍需长期迭代 |
| 防外挂 | 框架 | 框架 | Warden+行为分析，仍需补课 |
| Lua集成 | 热更 | 热更 | 全UI+Addon API，定位差异 |
| 测试成熟度 | 框架 | 框架 | 20年积累，仍需长期积累 |

**结论**: 优化后，CAMI在6个维度上基本对齐WoW水平，剩余4个维度（战斗深度/防外挂/Lua集成/测试成熟度）属于需要长期迭代的业务深度差距，不在架构层面解决。

---

> **文档维护**: 本文档为CAMI v2.0.0架构优化的核心设计文档，所有模块设计文档应参考本文档中的接口定义和参数。
