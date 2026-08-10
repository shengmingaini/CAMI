# CAMI v3.0 高并发优化设计 — 5万在线延迟与压力优化

> **文档状态**: [PRODUCTION]  
> **版本**: v3.0.0  
> **更新日期**: 2026-08-06  
> **符合5万在线架构**: 是  
> **变更范围**: 6个高并发瓶颈领域的系统性优化，目标降低通信延迟50%+、提升单节点吞吐2-3倍

---

## 0. 瓶颈分析

### 0.1 5万并发下的6大瓶颈

```
                     5万玩家
                        │
          ┌─────────────┼──────────────┐
          │             │              │
     10个Gateway   50个GameNode    8个DataService
     各5000连接    各1000人       各6250玩家
          │             │              │
    ┌─────▼─────┐  ┌────▼────┐  ┌─────▼─────┐
    │瓶颈1:     │  │瓶颈2:   │  │瓶颈3:     │
    │Gateway↔   │  │GameNode │  │DataService│
    │GameNode   │  │单线程   │  │序列化瓶颈 │
    │gRPC 500+  │  │所有任务 │  │所有DB读写 │
    │连接       │  │竞争主循 │  │经此代理   │
    │~1ms/跳    │  │环5ms    │  │           │
    └───────────┘  └─────────┘  └───────────┘
          │             │              │
    ┌─────▼─────┐  ┌────▼────┐  ┌─────▼─────┐
    │瓶颈4:     │  │瓶颈5:   │  │瓶颈6:     │
    │客户端↔    │  │负载超85%│  │AOI跨节点  │
    │Gateway    │  │硬性降级 │  │Redis Pub/ │
    │TCP握手    │  │无渐进式 │  │Sub消息    │
    │移动端延迟 │  │背压机制 │  │放大       │
    └───────────┘  └─────────┘  └───────────┘
```

### 0.2 优化目标

| 维度 | v2.0 现状 | v3.0 目标 | 提升幅度 |
|------|----------|----------|---------|
| Gateway↔GameNode延迟 | ~1ms (gRPC) | ~0.02ms (共享内存) | **-98%** |
| GameNode有效吞吐 | ~8000条/s (单线程) | ~20000条/s (多线程) | **+150%** |
| Redis访问跳数 | GameNode→DataService→Redis (2跳) | GameNode→Redis (1跳直连) | **-50%** |
| 客户端连接建立 | TCP 3次握手 (~100ms) | QUIC 0-RTT (~0ms) | **-100%** |
| 过载保护 | 85%硬性降级 | L1~L4渐进式 (70%→100%) | **质变** |
| 跨节点AOI消息量 | 每实体1条Pub/Sub | 聚合+差分 (-80%) | **-80%** |

---

## 1. 进程间通信优化 (IPC)

### 1.1 问题分析

v2.0架构中Gateway↔GameNode使用gRPC：

```
10 Gateway × 50 GameNode = 500+ gRPC连接
每条消息: 序列化 → HTTP/2帧 → TCP → 反序列化 ≈ 1ms
峰值: 50节点 × 8000条/s = 400,000条/s 集群总量
gRPC开销占比: 1ms / (1ms + 0.01ms处理) ≈ 99% 是网络开销
```

### 1.2 三层IPC方案

```
┌─────────────────────────────────────────────────────────────┐
│                    IPC 协议选择矩阵                          │
├──────────────┬──────────────┬───────────────────────────────┤
│ 场景         │ 协议         │ 延迟    │ 说明                 │
├──────────────┼──────────────┼───────────────────────────────┤
│ 同K8s节点    │ 共享内存SHM  │ ~0.02ms │ Gateway和GameNode    │
│ (co-located) │ + RingBuffer │         │ 在同一Pod/Node       │
├──────────────┼──────────────┼───────────────────────────────┤
│ 同集群不同节点│ UDS / 本地TCP│ ~0.1ms  │ K8s节点间内网        │
├──────────────┼──────────────┼───────────────────────────────┤
│ 跨集群/跨区域│ gRPC (批量)  │ ~2ms    │ 跨服/跨区域通信      │
└──────────────┴──────────────┴───────────────────────────────┘
```

### 1.3 共享内存 RingBuffer 设计

```cpp
// [PRODUCTION] 共享内存IPC — 同节点Gateway↔GameNode零拷贝通信
class SharedMemoryIPC {
public:
    // 初始化共享内存区域（Gateway和GameNode共享）
    bool init(const std::string& shm_name, size_t buffer_size = 64 * 1024 * 1024);

    // Gateway → GameNode: 写入消息（零拷贝）
    bool sendToGameNode(uint32_t game_node_id, 
                        const uint8_t* data, size_t len) {
        auto& ring = m_send_rings[game_node_id];
        
        // 1. 检查RingBuffer可用空间
        if (ring.available() < len + sizeof(MessageHeader)) {
            // 缓冲区满，触发背压
            m_backpressure_counter++;
            return false;
        }
        
        // 2. 写入消息头 + 消息体（直接写入共享内存，零拷贝）
        MessageHeader header{len, m_seq++, steady_clock::now()};
        ring.write(&header, sizeof(header));
        ring.write(data, len);
        
        // 3. 内存屏障确保GameNode可见
        std::atomic_thread_fence(std::memory_order_release);
        
        return true;
    }

    // GameNode: 批量读取所有待处理消息
    void drainMessages(std::vector<Message>& out_messages) {
        for (auto& [node_id, ring] : m_recv_rings) {
            while (ring.available_read() > 0) {
                MessageHeader header;
                ring.peek(&header, sizeof(header));
                
                if (ring.available_read() < sizeof(header) + header.len)
                    break;  // 不完整消息，等下次
                
                Message msg;
                msg.data.resize(header.len);
                ring.read(&header, sizeof(header));
                ring.read(msg.data.data(), header.len);
                msg.seq = header.seq;
                msg.timestamp = header.timestamp;
                out_messages.push_back(std::move(msg));
            }
        }
    }

private:
    struct MessageHeader {
        uint32_t len;
        uint64_t seq;
        int64_t timestamp;  // steady_clock
    };

    // SPSC RingBuffer per GameNode
    // [EXPANSION_RISK] 50个RingBuffer × 64MB = 3.2GB共享内存
    // 生产环境可按需分配，每个RingBuffer默认16MB
    std::unordered_map<uint32_t, SpscRingBuffer> m_send_rings;
    std::unordered_map<uint32_t, SpscRingBuffer> m_recv_rings;
    std::atomic<uint64_t> m_seq{0};
    std::atomic<uint64_t> m_backpressure_counter{0};
};

// [PRODUCTION] 无锁SPSC RingBuffer
class SpscRingBuffer {
public:
    bool init(void* addr, size_t size);
    size_t available() const;
    size_t available_read() const;
    bool write(const void* data, size_t len);
    bool read(void* data, size_t len);
    bool peek(void* data, size_t len) const;
    
private:
    // 缓存行隔离避免false sharing
    alignas(64) std::atomic<size_t> m_write_pos{0};
    alignas(64) std::atomic<size_t> m_read_pos{0};
    alignas(64) uint8_t* m_buffer;
    size_t m_capacity;
    size_t m_mask;  // capacity - 1, 要求capacity是2的幂
};
```

### 1.4 K8s 亲和性部署

```yaml
# K8s部署: Gateway和GameNode同节点亲和
apiVersion: apps/v1
kind: Deployment
metadata:
  name: gamenode
spec:
  template:
    spec:
      affinity:
        podAffinity:
          requiredDuringSchedulingIgnoredDuringExecution:
          - labelSelector:
              matchExpressions:
              - key: app
                operator: In
                values: ["gateway"]
            topologyKey: kubernetes.io/hostname
      containers:
      - name: gamenode
        resources:
          limits:
            memory: 16Gi
            cpu: 8
          # 共享内存通过emptyDir挂载
          volumes:
          - name: shm
            emptyDir:
              medium: Memory  # tmpfs (RAM-backed)
              sizeLimit: 1Gi
```

### 1.5 批量gRPC（跨节点场景）

```cpp
// [PRODUCTION] 批量gRPC — 跨节点消息聚合发送
class BatchedGrpcClient {
public:
    // 提交消息（不立即发送，加入缓冲区）
    void submit(uint32_t target_node, const uint8_t* data, size_t len) {
        m_buffers[target_node].push(data, len);
    }

    // 定时刷新（每5ms或缓冲区满时批量发送）
    void flush() {
        for (auto& [node, buffer] : m_buffers) {
            if (buffer.empty()) continue;
            
            // 聚合为一条gRPC消息
            BatchMessage batch;
            batch.set_node_id(m_local_node_id);
            for (auto& msg : buffer) {
                batch.add_messages(msg.data(), msg.size());
            }
            
            // 异步发送
            m_stubs[node]->AsyncBatchSend(&batch, [node](const Status& s) {
                if (!s.ok()) {
                    // 失败重试或降级到Redis
                }
            });
            
            buffer.clear();
        }
    }

private:
    static constexpr int64_t FLUSH_INTERVAL_MS = 5;
    static constexpr size_t MAX_BATCH_SIZE = 64 * 1024;  // 64KB
    
    // [EXPANSION_RISK] 5ms刷新间隔增加端到端延迟
    // 但聚合后吞吐量提升10x+，总体延迟反而降低
    std::unordered_map<uint32_t, MessageBuffer> m_buffers;
    std::unordered_map<uint32_t, std::unique_ptr<GrpcStub>> m_stubs;
};
```

### 1.6 性能预估

| 场景 | v2.0 (gRPC) | v3.0 (SHM) | 提升 |
|------|------------|-----------|------|
| 同节点消息延迟 | 1.0ms | 0.02ms | **50x** |
| 单节点IPC吞吐 | 8,000条/s | 500,000条/s | **62x** |
| CPU开销(序列化) | ~5% | ~0% (零拷贝) | **-100%** |
| 内存开销 | gRPC连接池 | 16MB RingBuffer | 可控 |

---

## 2. GameNode 内部并发架构

### 2.1 问题分析

v2.0架构中GameNode是单线程主循环：

```
每帧(33ms)需要完成:
  - 战斗循环(5ms)
  - AOI更新(?ms)
  - 移动验证(?ms)
  - 寻路计算(?ms)
  - 社交/任务/经济(?ms)
  - 消息收发(?ms)
  
  总计可能超过33ms → 帧率下降 → 玩家卡顿
  
  8核CPU利用率: ~12.5% (1/8核)
```

### 2.2 场景级线程隔离 (Scene-per-Thread)

```cpp
// [PRODUCTION] 场景线程模型 — 每个场景独立线程
class SceneThread {
public:
    SceneThread(uint32_t scene_id, uint32_t layer_id, uint32_t phase_id)
        : m_scene_id(scene_id), m_layer_id(layer_id), m_phase_id(phase_id) {
        m_thread = std::thread(&SceneThread::run, this);
    }

    void run() {
        while (m_running) {
            auto frame_start = steady_clock::now();
            
            // 1. 读取本场景消息（从共享内存IPC）
            m_ipc.drainMessages(m_pending_messages);
            
            // 2. 战斗循环（本场景内所有玩家）
            m_combat_system.tick(frame_delta);
            
            // 3. AOI更新（本场景视野计算）
            m_aoi_manager.tick(frame_start);
            
            // 4. 移动验证 + 预测纠偏
            m_prediction_system.tick(frame_delta);
            
            // 5. 寻路处理（本帧配额）
            m_nav_scheduler.tick();
            
            // 6. 发送AOI更新到Gateway
            m_ipc.sendUpdates(m_aoi_manager.getPendingUpdates());
            
            // 7. 非关键任务投递到Job System
            for (auto& task : m_deferred_tasks) {
                g_job_system.submit(std::move(task));
            }
            
            // 8. 等待下一帧
            auto elapsed = steady_clock::now() - frame_start;
            if (elapsed < FRAME_TIME) {
                std::this_thread::sleep_for(FRAME_TIME - elapsed);
            }
        }
    }

private:
    static constexpr int64_t FRAME_TIME = 33;  // 30fps
    
    uint32_t m_scene_id;
    uint32_t m_layer_id;
    uint32_t m_phase_id;
    std::thread m_thread;
    std::atomic<bool> m_running{true};
    
    // 本场景独立的游戏系统实例
    CombatSystem m_combat_system;
    AOIManager m_aoi_manager;
    PredictionSystem m_prediction_system;
    PathfindScheduler m_nav_scheduler;
    
    SharedMemoryIPC m_ipc;  // IPC接口
    std::vector<Task> m_deferred_tasks;
};

// [PRODUCTION] GameNode 线程管理器
class GameNodeThreadManager {
public:
    // 玩家进入场景时分配到对应场景线程
    void assignPlayer(uint64_t player_id, uint32_t scene_id, 
                      uint32_t layer, uint32_t phase) {
        uint64_t thread_key = makeKey(scene_id, layer, phase);
        m_scene_threads[thread_key]->addPlayer(player_id);
        m_player_to_thread[player_id] = thread_key;
    }

    // 玩家跨场景时线程切换
    void transferPlayer(uint64_t player_id, uint32_t new_scene,
                        uint32_t new_layer, uint32_t new_phase) {
        uint64_t old_key = m_player_to_thread[player_id];
        uint64_t new_key = makeKey(new_scene, new_layer, new_phase);
        
        // 1. 从旧线程移除
        m_scene_threads[old_key]->removePlayer(player_id);
        
        // 2. 序列化玩家状态到共享内存
        auto state = m_scene_threads[old_key]->serializePlayer(player_id);
        m_shared_state.set(player_id, state);
        
        // 3. 新线程加载玩家状态
        m_scene_threads[new_key]->addPlayer(player_id);
        m_scene_threads[new_key]->loadPlayerState(player_id, state);
        
        m_player_to_thread[player_id] = new_key;
    }

private:
    // scene_id + layer + phase → 线程
    std::unordered_map<uint64_t, std::unique_ptr<SceneThread>> m_scene_threads;
    std::unordered_map<uint64_t, uint64_t> m_player_to_thread;
    
    // [EXPANSION_RISK] 线程数 = 场景数 × 层数 × 相位数
    // 1000人节点通常管理 5-10个活跃场景 = 5-10线程
    // 8核CPU可轻松承载，CPU密集型场景可设上限
    static constexpr uint32_t MAX_SCENE_THREADS = 16;
};
```

### 2.3 Job System（非关键任务并行化）

```cpp
// [PRODUCTION] Job System — 非关键任务工作线程池
class JobSystem {
public:
    static JobSystem& instance() {
        static JobSystem s;
        return s;
    }

    // 提交任务（非关键路径: 邮件/任务进度/日志/排行榜更新）
    template<typename F>
    void submit(F&& task, JobPriority priority = JobPriority::NORMAL) {
        m_queues[priority].push(std::forward<F>(task));
    }

    // 工作线程主循环
    void workerLoop() {
        while (m_running) {
            // 优先级队列: HIGH > NORMAL > LOW
            Task task;
            if (m_queues[JobPriority::HIGH].tryPop(task) ||
                m_queues[JobPriority::NORMAL].tryPop(task) ||
                m_queues[JobPriority::LOW].tryPop(task)) {
                task();
            } else {
                std::this_thread::yield();
            }
        }
    }

private:
    JobSystem() {
        uint32_t num_workers = std::thread::hardware_concurrency() - 1;  // 留1核给场景线程
        for (uint32_t i = 0; i < num_workers; ++i) {
            m_workers.emplace_back(&JobSystem::workerLoop, this);
        }
    }

    enum class JobPriority { HIGH, NORMAL, LOW };
    
    LockFreeQueue<Task> m_queues[3];
    std::vector<std::thread> m_workers;
    std::atomic<bool> m_running{true};
};
```

### 2.4 ECS 数据布局优化

```cpp
// [PRODUCTION] ECS — 缓存友好的实体数据布局

// v2.0: Array of Structures (AoS) — 缓存不友好
// struct Player { Vector3 pos; int32_t hp; int32_t mp; BuffList buffs; ... };
// 遍历1000人时: 每次cache line只用到部分字段

// v3.0: Structure of Arrays (SoA) — 缓存友好
class PlayerComponentStorage {
public:
    // 位置数据连续存储（战斗循环批量读取）
    struct alignas(64) PositionData {
        std::vector<float> x, y, z;   // 连续内存
        std::vector<uint8_t> facing;   // 朝向
    };
    
    // 战斗属性连续存储
    struct alignas(64) CombatData {
        std::vector<int32_t> hp, max_hp;
        std::vector<int32_t> mp, max_mp;
        std::vector<uint16_t> buff_flags;
        std::vector<uint8_t> combat_state;
    };
    
    // 非高频数据分开存储
    struct alignas(64) InventoryData {
        std::vector<Inventory> inventories;  // 不在战斗循环中遍历
    };

    PositionData m_positions;    // 战斗循环批量读
    CombatData m_combat;         // 战斗循环批量读
    InventoryData m_inventory;   // 仅背包操作时访问
    
    // [EXPANSION_RISK] 数据迁移开销: AoS→SoA需要重构模块
    // 建议在模块设计阶段就采用ECS，而非后期重构
};

// 战斗循环批量处理（SIMD友好）
void CombatSystem::tick(float delta) {
    auto& storage = m_component_storage;
    size_t count = storage.activeCount();
    
    // 批量读取所有玩家位置（连续内存，cache命中率高）
    const float* px = storage.m_positions.x.data();
    const float* py = storage.m_positions.y.data();
    const float* pz = storage.m_positions.z.data();
    
    // 批量处理伤害结算
    for (size_t i = 0; i < count; ++i) {
        // AoE检查: 所有玩家位置连续存储，SIMD可并行计算距离
        // cache miss率从~60%降至~10%
    }
}
```

### 2.5 线程间无锁通信

```cpp
// [PRODUCTION] 无锁MPSC队列 — 场景线程间消息传递
template<typename T>
class MPMCQueue {
public:
    bool push(T item) {
        Node* node = m_pool.acquire();
        node->data = std::move(item);
        
        Node* prev = m_tail.exchange(node, std::memory_order_acq_rel);
        prev->next = node;
        return true;
    }
    
    bool pop(T& out) {
        Node* head = m_head.load(std::memory_order_acquire);
        Node* next = head->next.load(std::memory_order_acquire);
        
        if (!next) return false;
        
        out = std::move(next->data);
        m_head.store(next, std::memory_order_release);
        m_pool.release(head);
        return true;
    }

private:
    struct Node {
        T data;
        std::atomic<Node*> next{nullptr};
    };
    
    alignas(64) std::atomic<Node*> m_head;
    alignas(64) std::atomic<Node*> m_tail;
    ObjectPool<Node> m_pool;  // 预分配节点池
};
```

### 2.6 并发架构性能预估

| 指标 | v2.0 (单线程) | v3.0 (场景线程) | 提升 |
|------|-------------|----------------|------|
| CPU利用率 | ~12.5% (1/8核) | ~75% (6/8核) | **6x** |
| 有效消息吞吐 | 8,000条/s | ~20,000条/s | **2.5x** |
| 战斗循环cache miss率 | ~60% | ~10% (ECS) | **-83%** |
| 非关键任务延迟 | 阻塞主循环 | 异步并行 | **不阻塞** |

---

## 3. 三级缓存与数据层优化

### 3.1 问题分析

v2.0架构数据访问路径：

```
GameNode → gRPC → DataService → Redis/MySQL
         ~2ms      ~1ms        ~1ms
总延迟: ~3-4ms per read

1000人 × 10 reads/s = 10,000 reads/s per node
50 nodes × 10,000 = 500,000 reads/s through DataService
8 DataService × 62,500 reads/s each → 超负荷
```

### 3.2 三级缓存架构

```
┌─────────────────────────────────────────────────────────────┐
│                     GameNode (进程内)                        │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  L1 Cache (进程内内存)                                │    │
│  │  ├─ 玩家属性快照 (活跃玩家全量)    TTL: 实时          │    │
│  │  ├─ 场景配置数据                  TTL: 永久(直到更新) │    │
│  │  ├─ NavMesh数据                  TTL: 场景存活期     │    │
│  │  └─ 静态配置表(技能/物品/怪物)    TTL: 热更时失效     │    │
│  │  命中率目标: >95%                                    │    │
│  │  容量: ~2GB                                          │    │
│  └───────────────────────┬─────────────────────────────┘    │
│                          │ miss                             │
└──────────────────────────┼──────────────────────────────────┘
                           │
┌──────────────────────────┼──────────────────────────────────┐
│                     Redis Cluster (L2)                       │
│  ┌───────────────────────▼─────────────────────────────┐    │
│  │  L2 Cache (Redis Cluster 16分片)                     │    │
│  │  ├─ 玩家完整数据 (在线/离线玩家)                      │    │
│  │  ├─ 背包/货币/Buff                                   │    │
│  │  ├─ 连接上下文 (迁移用)                               │    │
│  │  ├─ 场景AOI缓存                                      │    │
│  │  └─ 分布式锁/版本号                                  │    │
│  │  命中率目标: >80% (L1 miss后)                        │    │
│  │  容量: ~512GB (16 × 32GB)                           │    │
│  └───────────────────────┬─────────────────────────────┘    │
│                          │ miss                             │
└──────────────────────────┼──────────────────────────────────┘
                           │
┌──────────────────────────┼──────────────────────────────────┐
│                     MySQL (L3)                               │
│  ┌───────────────────────▼─────────────────────────────┐    │
│  │  L3 持久化存储 (MySQL 8分库 + 独立库)                │    │
│  │  ├─ 玩家完整数据 (持久化)                             │    │
│  │  ├─ 拍卖行/公会 (独立库)                              │    │
│  │  └─ 写入: Write-Behind + WAL                         │    │
│  │  读取: 仅冷数据(L2 miss) + 写副本查询                  │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

### 3.3 L1 进程内缓存

```cpp
// [PRODUCTION] L1进程内缓存 — 减少Redis往返
class L1Cache {
public:
    // 玩家上线时加载到L1
    void loadPlayer(uint64_t player_id) {
        if (m_player_cache.find(player_id) != m_player_cache.end())
            return;  // 已在缓存中
        
        // 从Redis加载
        auto data = m_redis.get(playerKey(player_id));
        if (data) {
            m_player_cache[player_id] = deserialize(*data);
            m_player_cache[player_id].last_access = steady_clock::now();
        }
    }

    // 读取玩家数据（L1优先）
    const PlayerData* get(uint64_t player_id) {
        auto it = m_player_cache.find(player_id);
        if (it != m_player_cache.end()) {
            it->second.last_access = steady_clock::now();
            m_hit_count++;
            return &it->second.data;
        }
        m_miss_count++;
        return nullptr;  // 需要从L2加载
    }

    // 写入玩家数据（写穿透L1→L2，异步落L3）
    void put(uint64_t player_id, PlayerData data) {
        m_player_cache[player_id].data = std::move(data);
        m_player_cache[player_id].last_access = steady_clock::now();
        m_player_cache[player_id].dirty = true;
        
        // 同步写L2 (Redis)
        m_redis.set(playerKey(player_id), serialize(data));
    }

    // 定期将脏数据批量落L3 (MySQL)
    void flushDirty() {
        std::vector<std::pair<uint64_t, PlayerData>> dirty;
        for (auto& [id, entry] : m_player_cache) {
            if (entry.dirty) {
                dirty.push_back({id, entry.data});
                entry.dirty = false;
            }
        }
        if (!dirty.empty()) {
            // 批量写入MySQL（走DataService gRPC）
            m_data_service.batchUpdate(dirty);
        }
    }

    // LRU淘汰（缓存容量超限时）
    void evictIfNeeded() {
        if (m_player_cache.size() <= MAX_ENTRIES) return;
        
        // 淘汰最久未访问的非活跃玩家
        size_t to_evict = m_player_cache.size() - MAX_ENTRIES;
        std::vector<std::pair<uint64_t, int64_t>> access_times;
        for (auto& [id, entry] : m_player_cache) {
            access_times.push_back({id, entry.last_access.time_since_epoch().count()});
        }
        std::partial_sort(access_times.begin(), 
                         access_times.begin() + to_evict,
                         access_times.end(),
                         [](auto& a, auto& b) { return a.second < b.second; });
        
        for (size_t i = 0; i < to_evict; ++i) {
            uint64_t id = access_times[i].first;
            if (m_player_cache[id].dirty) {
                // 脏数据先落L2再淘汰
                m_redis.set(playerKey(id), serialize(m_player_cache[id].data));
            }
            m_player_cache.erase(id);
        }
    }

private:
    static constexpr size_t MAX_ENTRIES = 1500;  // 1000活跃 + 500预加载
    
    struct CacheEntry {
        PlayerData data;
        int64_t last_access;
        bool dirty{false};
    };
    
    std::unordered_map<uint64_t, CacheEntry> m_player_cache;
    RedisClusterClient m_redis;  // 直连Redis（绕过DataService）
    DataServiceClient m_data_service;  // 仅MySQL写入走DataService
};
```

### 3.4 Redis 直连分片（绕过DataService）

```cpp
// [PRODUCTION] Redis Cluster直连客户端 — GameNode绕过DataService直接读写Redis
class RedisDirectClient {
public:
    RedisDirectClient() {
        // 初始化Redis Cluster客户端（自动分片路由）
        m_cluster = std::make_unique<redisAsyncCluster>();
        m_cluster->initShards(getRedisShardNodes());  // 16分片节点列表
        
        // Pipeline批量操作
        m_pipeline.setBatchSize(64);  // 每批最多64条命令
        m_pipeline.setFlushInterval(1);  // 1ms刷新
    }

    // 批量读取（Pipeline）
    std::vector<std::optional<PlayerData>> batchGet(
        const std::vector<uint64_t>& player_ids) {
        
        // 1. 按分片分组
        std::unordered_map<uint16_t, std::vector<uint64_t>> by_shard;
        for (auto pid : player_ids) {
            uint16_t shard = crc16(pid) % 16;
            by_shard[shard].push_back(pid);
        }
        
        // 2. 每个分片Pipeline批量读取
        std::vector<std::optional<PlayerData>> results(player_ids.size());
        for (auto& [shard, ids] : by_shard) {
            auto replies = m_cluster[shard].pipelineGet(
                ids | std::views::transform(playerKey));
            
            for (size_t i = 0; i < ids.size(); ++i) {
                if (replies[i]) {
                    results[i] = deserialize(*replies[i]);
                }
            }
        }
        
        return results;
    }

    // 异步写入（不阻塞场景线程）
    void asyncSet(uint64_t player_id, const PlayerData& data) {
        m_cluster.asyncSet(playerKey(player_id), serialize(data));
    }

private:
    std::unique_ptr<redisAsyncCluster> m_cluster;
    PipelineConfig m_pipeline;
    
    std::string playerKey(uint64_t pid) {
        return fmt::format("player:{}", pid);
    }
    
    uint16_t crc16(uint64_t pid) {
        // CRC16计算分片
    }
};
```

### 3.5 Write-Behind + WAL

```cpp
// [PRODUCTION] Write-Behind + WAL — 替代30s批量flush
class WriteBehindManager {
public:
    // 玩家数据变更时记录WAL
    void onChange(uint64_t player_id, const PlayerData& new_data, 
                  uint32_t version) {
        // 1. 写入WAL（顺序追加，fsync保证持久性）
        m_wal.append(WalEntry{
            .player_id = player_id,
            .version = version,
            .data = new_data,
            .timestamp = now()
        });
        
        // 2. 更新L1缓存
        m_l1_cache.put(player_id, new_data);
        
        // 3. 异步写Redis（L2）
        m_redis.asyncSet(playerKey(player_id), serialize(new_data));
        
        // WAL保证: 即使L1和L2都丢失，WAL也能恢复数据
    }

    // WAL检查点：定期将WAL中的变更批量落MySQL
    void checkpoint() {
        // 1. 读取WAL中所有未落库的变更
        auto entries = m_wal.readSince(m_last_checkpoint);
        
        // 2. 按分片分组
        std::unordered_map<uint16_t, std::vector<WalEntry>> by_shard;
        for (auto& e : entries) {
            by_shard[crc16(e.player_id) % 8].push_back(e);
        }
        
        // 3. 每个分片批量UPDATE
        for (auto& [shard, entries] : by_shard) {
            m_mysql[shard].batchUpsert(entries);
        }
        
        // 4. 标记WAL已落库位置
        m_last_checkpoint = now();
        m_wal.truncate(m_last_checkpoint);
    }

    // 崩溃恢复：从WAL重放未落库的变更
    void recover() {
        auto entries = m_wal.readSince(m_last_checkpoint);
        for (auto& e : entries) {
            // 重放到Redis和MySQL
            m_redis.set(playerKey(e.player_id), serialize(e.data));
            m_mysql[crc16(e.player_id) % 8].upsert(e);
        }
    }

private:
    WriteAheadLog m_wal;  // 预写日志
    L1Cache m_l1_cache;
    RedisDirectClient m_redis;
    MySQLProxy m_mysql[8];
    int64_t m_last_checkpoint{0};
    
    static constexpr int64_t CHECKPOINT_INTERVAL_MS = 5000;  // 5秒检查点
    // [EXPANSION_RISK] WAL文件增长速度取决于写入频率
    // 1000人 × 10 writes/s = 10,000 WAL entries/s
    // 5秒 = 50,000 entries ≈ 50MB WAL文件
};
```

### 3.6 MySQL 读写分离

```cpp
// [PRODUCTION] MySQL读写分离 — 查询类操作走读副本
class MySQLReadWriteSplit {
public:
    // 写入：走主库（通过DataService）
    void write(uint64_t player_id, const PlayerData& data) {
        uint16_t shard = crc16(player_id) % 8;
        m_master[shard].upsert(player_id, data);
    }

    // 读取：优先走读副本（降低主库压力）
    PlayerData read(uint64_t player_id) {
        uint16_t shard = crc16(player_id) % 8;
        
        // 读副本有~100ms复制延迟，仅用于非关键读取
        // 关键读取（登录/交易）仍走主库
        auto data = m_replica[shard].get(player_id);
        if (data) return *data;
        
        // 读副本未同步，回退到主库
        return m_master[shard].get(player_id);
    }

    // 批量查询（排行榜/拍卖行浏览等非关键场景）
    std::vector<PlayerData> batchRead(
        const std::vector<uint64_t>& player_ids) {
        // 全部走读副本
        std::unordered_map<uint16_t, std::vector<uint64_t>> by_shard;
        for (auto pid : player_ids) {
            by_shard[crc16(pid) % 8].push_back(pid);
        }
        
        std::vector<PlayerData> results;
        for (auto& [shard, ids] : by_shard) {
            auto shard_results = m_replica[shard].batchGet(ids);
            results.insert(results.end(), 
                          shard_results.begin(), shard_results.end());
        }
        return results;
    }

private:
    MySQLClient m_master[8];    // 8个主库分片
    MySQLClient m_replica[8];   // 8个读副本（每分片1副本）
};
```

### 3.7 数据层性能预估

| 场景 | v2.0 | v3.0 | 提升 |
|------|------|------|------|
| 热数据读取延迟 | ~3ms (gRPC→Redis) | ~0.001ms (L1命中) | **3000x** |
| 冷数据读取延迟 | ~5ms (gRPC→Redis→MySQL) | ~2ms (Redis直连→MySQL) | **2.5x** |
| 数据写入延迟 | ~3ms (同步gRPC→MySQL) | ~0.1ms (WAL异步) | **30x** |
| DataService负载 | 所有读写 | 仅MySQL写入 | **-90%** |
| 崩溃恢复 | 丢失30s数据 | WAL恢复0丢失 | **质变** |

---

## 4. 网络协议与边缘接入优化

### 4.1 QUIC 协议替代 TCP

```
TCP的问题 (5万并发):
  - 3次握手: ~100ms首包延迟（移动端更慢）
  - 队头阻塞: 一个包丢失阻塞整个连接
  - 连接迁移: IP变化时断开重连
  - 拥塞控制: 粗粒度，不适合游戏混合流量

QUIC优势:
  - 0-RTT连接恢复: 重连玩家~0ms建立连接
  - 无队头阻塞: 流级别独立，丢包不影响其他流
  - 连接迁移: IP变化不断连（移动端切换WiFi/4G）
  - 独立流控: 可靠流(TCP语义) + 不可靠流(UDP语义)共存
```

```cpp
// [PRODUCTION] QUIC传输层 — 替代TCP+UDP双通道
class QuicTransport {
public:
    // 初始化QUIC监听
    bool listen(uint16_t port) {
        m_quic = quiche_conn_new(port, &m_config);
        // 配置: 支持0-RTT, 双向流, 不可靠数据报
        quiche_config_set_max_idle_timeout(&m_config, 30000);
        quiche_config_set_max_recv_udp_payload_size(&m_config, 1450);
        quiche_config_enable_dgram(&m_config, true, 1024, 1024);
        return true;
    }

    // 发送高频消息（不可靠数据报，类似UDP）
    void sendUnreliable(uint64_t conn_id, 
                        const uint8_t* data, size_t len) {
        // QUIC DATAGRAM: 不保证到达，不阻塞可靠流
        quiche_conn_dgram_send(m_conns[conn_id], data, len);
    }

    // 发送可靠消息（QUIC流，类似TCP）
    void sendReliable(uint64_t conn_id,
                      const uint8_t* data, size_t len) {
        // QUIC STREAM: 保证顺序到达
        quiche_conn_stream_send(m_conns[conn_id], 
                               m_next_stream_id++, data, len, true);
    }

    // 0-RTT重连
    void onEarlyData(uint64_t conn_id, const uint8_t* data, size_t len) {
        // 玩家重连时0-RTT数据直接处理
        // 无需等待握手完成
        processGameMessage(conn_id, data, len);
    }

private:
    quiche_config m_config;
    std::unordered_map<uint64_t, quiche_conn*> m_conns;
    uint64_t m_next_stream_id{0};
};
```

### 4.2 边缘网关部署

```
┌─────────────────────────────────────────────────────────────┐
│                     边缘网关架构                              │
│                                                             │
│  玩家(华东)    玩家(华南)    玩家(华北)    玩家(海外)        │
│      │            │            │            │               │
│  ┌───▼───┐   ┌───▼───┐   ┌───▼───┐   ┌───▼───┐           │
│  │边缘GW  │   │边缘GW  │   │边缘GW  │   │边缘GW  │           │
│  │(上海)  │   │(深圳)  │   │(北京)  │   │(香港)  │           │
│  │~5ms    │   │~5ms    │   │~5ms    │   │~30ms   │           │
│  └───┬───┘   └───┬───┘   └───┬───┘   └───┬───┘           │
│      │            │            │            │               │
│      └────────────┴─────┬──────┴────────────┘               │
│                          │                                   │
│                   ┌──────▼──────┐                           │
│                   │  中心集群     │                           │
│                   │  (K8s)       │                           │
│                   │  50 GameNode │                           │
│                   │  16 Redis    │                           │
│                   │  8 MySQL     │                           │
│                   └─────────────┘                           │
└─────────────────────────────────────────────────────────────┘

边缘网关职责:
  - 就近接入: 玩家连接最近的边缘网关，降低RTT
  - 协议转换: QUIC↔内部IPC
  - 消息聚合: 多个小消息合并发送到中心集群
  - 本地缓存: 静态配置数据本地缓存
  - 限流防护: DDoS防护第一道防线
```

### 4.3 消息聚合

```cpp
// [PRODUCTION] 消息聚合器 — 多个小消息合并发送
class MessageAggregator {
public:
    // 添加消息到聚合缓冲区
    void add(uint64_t conn_id, const uint8_t* data, size_t len) {
        auto& buf = m_buffers[conn_id];
        
        // 如果单消息超过MTU，直接发送（不聚合）
        if (len > LARGE_MESSAGE_THRESHOLD) {
            flush(conn_id);
            sendDirect(conn_id, data, len);
            return;
        }
        
        // 添加到聚合缓冲区
        buf.write(data, len);
        buf.message_count++;
        
        // 缓冲区满或消息数达标，触发刷新
        if (buf.size() >= MAX_BUFFER_SIZE || 
            buf.message_count >= MAX_MESSAGE_COUNT) {
            flush(conn_id);
        }
    }

    // 定时刷新（每3ms或下一帧）
    void flushAll() {
        for (auto& [conn_id, buf] : m_buffers) {
            if (buf.message_count > 0) {
                flush(conn_id);
            }
        }
    }

    void flush(uint64_t conn_id) {
        auto& buf = m_buffers[conn_id];
        if (buf.message_count == 0) return;
        
        // 一次性发送聚合后的数据
        m_transport.sendUnreliable(conn_id, buf.data(), buf.size());
        buf.clear();
    }

private:
    static constexpr size_t MAX_BUFFER_SIZE = 1200;    // MTU以下
    static constexpr uint32_t MAX_MESSAGE_COUNT = 20;   // 最多20条
    static constexpr int64_t FLUSH_INTERVAL_MS = 3;     // 3ms定时刷新
    
    struct AggregationBuffer {
        std::vector<uint8_t> buffer;
        uint32_t message_count{0};
        // ...
    };
    
    std::unordered_map<uint64_t, AggregationBuffer> m_buffers;
};
```

### 4.4 连接亲和性路由

```cpp
// [PRODUCTION] 连接亲和性 — 玩家优先路由到上次GameNode
class AffinityRouter {
public:
    uint32_t routeGameNode(uint64_t player_id) {
        // 1. 检查亲和性记录
        auto it = m_affinity.find(player_id);
        if (it != m_affinity.end()) {
            uint32_t preferred_node = it->second;
            
            // 2. 检查目标节点是否健康且未满载
            if (isHealthy(preferred_node) && 
                getLoad(preferred_node) < LOAD_THRESHOLD) {
                return preferred_node;
            }
        }
        
        // 3. 回退到一致性哈希
        uint32_t node = consistentHash(player_id);
        
        // 4. 记录亲和性
        m_affinity[player_id] = node;
        
        return node;
    }

    // 玩家下线时清除亲和性（下次上线重新分配）
    void clearAffinity(uint64_t player_id) {
        m_affinity.erase(player_id);
    }

private:
    static constexpr float LOAD_THRESHOLD = 0.85;  // 85%负载
    
    // player_id → preferred_game_node
    // 存储在Redis中（跨Gateway共享）
    std::unordered_map<uint64_t, uint32_t> m_affinity;
};
```

### 4.5 网络层性能预估

| 指标 | v2.0 (TCP+UDP) | v3.0 (QUIC+边缘) | 提升 |
|------|---------------|-----------------|------|
| 首包延迟(重连) | ~100ms | ~0ms (0-RTT) | **-100%** |
| 移动端切换断连 | 是 | 否(连接迁移) | **质变** |
| 队头阻塞 | 有(TCP) | 无(QUIC流) | **消除** |
| 玩家到GW延迟 | ~30-50ms | ~5-10ms(边缘) | **-80%** |
| 小消息带宽利用率 | ~60% | ~90%(聚合) | **+50%** |

---

## 5. 负载管理与自适应降级体系

### 5.1 背压机制

```cpp
// [PRODUCTION] 背压系统 — GameNode过载时通知Gateway降速
class BackpressureSystem {
public:
    // 每帧检测负载
    void tick() {
        float load = calculateLoad();
        
        BackpressureLevel new_level = classifyLoad(load);
        
        if (new_level != m_current_level) {
            // 负载级别变化，通知所有关联Gateway
            notifyGateways(new_level);
            m_current_level = new_level;
            
            // 记录变更日志
            LOG_INFO("Backpressure level changed: {} -> {}", 
                     levelName(m_current_level), levelName(new_level));
        }
    }

    float calculateLoad() {
        // 综合负载 = CPU + 内存 + 消息队列 + 帧时间
        float cpu_load = getCpuUsage();
        float mem_load = getMemoryUsage() / MEM_LIMIT;
        float queue_load = (float)m_message_queue.size() / MAX_QUEUE;
        float frame_load = (float)m_last_frame_time / FRAME_BUDGET;
        
        return std::max({cpu_load, mem_load, queue_load, frame_load});
    }

    void notifyGateways(BackpressureLevel level) {
        BackpressureNotification notif{
            .node_id = m_node_id,
            .level = level,
            .timestamp = now()
        };
        
        // 通过IPC通知所有关联Gateway
        for (auto gw_id : m_connected_gateways) {
            m_ipc.sendToGateway(gw_id, serialize(notif));
        }
    }

private:
    enum class BackpressureLevel {
        GREEN,   // < 70%: 正常
        YELLOW,  // 70-85%: 降速非关键消息
        ORANGE,  // 85-95%: 关闭非关键功能
        RED      // > 95%: 仅处理关键消息(战斗/移动)
    };
    
    BackpressureLevel classifyLoad(float load) {
        if (load < 0.70f) return BackpressureLevel::GREEN;
        if (load < 0.85f) return BackpressureLevel::YELLOW;
        if (load < 0.95f) return BackpressureLevel::ORANGE;
        return BackpressureLevel::RED;
    }
};
```

### 5.2 多级自适应降级

```cpp
// [PRODUCTION] 多级降级管理器 — 渐进式功能裁剪
class AdaptiveDegradation {
public:
    // 根据负载级别执行降级/恢复
    void onBackpressureLevel(BackpressureLevel level) {
        switch (level) {
            case BackpressureLevel::GREEN:
                enableAllFeatures();
                break;
                
            case BackpressureLevel::YELLOW:
                // L1降级: 降低非关键频率
                m_aoi_manager.setLodBias(1.5f);      // LOD距离扩大1.5x
                m_nav_scheduler.setMaxPerTick(30);    // 寻路配额减半
                disableFeature("weather_effects");    // 关闭天气效果
                disableFeature("ambient_sounds");     // 关闭环境音效
                break;
                
            case BackpressureLevel::ORANGE:
                // L2降级: 关闭非关键功能
                m_aoi_manager.setLodBias(2.0f);      // LOD距离扩大2x
                m_aoi_manager.setLod2Interval(1000);  // LOD2更新降到1s
                disableFeature("pet_updates");        // 宠物不更新
                disableFeature("guild_chat");         // 公会聊天限频
                disableFeature("quest_progression");  // 任务进度暂停
                break;
                
            case BackpressureLevel::RED:
                // L3降级: 仅保留核心功能
                m_aoi_manager.setLodBias(3.0f);      // LOD距离扩大3x
                m_aoi_manager.disableLod0();          // 关闭LOD0(仅发LOD1/2)
                disableFeature("social");             // 社交全部暂停
                disableFeature("economy");            // 交易暂停
                disableFeature("navigation");         // 寻路暂停
                // 保留: 战斗/移动/AOI(降频)
                break;
        }
    }

private:
    void disableFeature(const std::string& feature) {
        m_disabled_features.insert(feature);
        m_event_bus.publish(FeatureDisabledEvent{feature});
    }
    
    void enableAllFeatures() {
        for (const auto& f : m_disabled_features) {
            m_event_bus.publish(FeatureEnabledEvent{f});
        }
        m_disabled_features.clear();
        m_aoi_manager.setLodBias(1.0f);
        m_nav_scheduler.setMaxPerTick(50);
    }
    
    std::unordered_set<std::string> m_disabled_features;
};
```

### 5.3 Cell-based 开放世界

```
传统场景管理:
  一个场景(1000m × 1000m) → 一个GameNode线程 → 1000人上限

Cell-based场景管理:
  一个大场景(5000m × 5000m) → 分割为25个Cell(1000m × 1000m)
  每个Cell → 独立GameNode线程 → 25个线程各处理40人 = 1000人/场景
  
  优势:
  - 大型开放世界无场景边界（玩家无感穿越Cell）
  - 每个Cell独立处理AOI/战斗/寻路
  - Cell间通信通过进程内消息队列
  - 热点Cell可迁移到其他GameNode（负载均衡）
```

```cpp
// [PRODUCTION] Cell管理器 — 大场景分Cell管理
class CellManager {
public:
    // 将大场景分割为Cell网格
    void initScene(uint32_t scene_id, float world_width, 
                   float world_height, float cell_size) {
        uint32_t cols = (uint32_t)(world_width / cell_size);
        uint32_t rows = (uint32_t)(world_height / cell_size);
        
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < cols; ++c) {
                uint32_t cell_id = r * cols + c;
                m_cells[scene_id][cell_id] = std::make_unique<SceneThread>(
                    scene_id, cell_id);
            }
        }
    }

    // 玩家移动时检查是否跨Cell
    void onPlayerMove(uint64_t player_id, const Vector3& new_pos) {
        uint32_t old_cell = m_player_cell[player_id];
        uint32_t new_cell = posToCell(new_pos);
        
        if (old_cell != new_cell) {
            // 跨Cell迁移
            transferPlayer(player_id, old_cell, new_cell);
        }
    }

    // Cell间玩家迁移
    void transferPlayer(uint64_t player_id, uint32_t from_cell, 
                        uint32_t to_cell) {
        // 1. 序列化玩家状态
        auto state = m_cells[m_scene][from_cell]->serializePlayer(player_id);
        
        // 2. 目标Cell预加载玩家（预取机制）
        m_cells[m_scene][to_cell]->preLoadPlayer(player_id, state);
        
        // 3. 源Cell移除玩家
        m_cells[m_scene][from_cell]->removePlayer(player_id);
        
        // 4. 更新映射
        m_player_cell[player_id] = to_cell;
        
        // 5. 通知AOI系统更新视野（跨Cell可见性）
        m_aoi_border.updateBorderVisibility(from_cell, to_cell, player_id);
    }

    // Cell负载均衡：过载Cell迁移到其他GameNode
    void rebalanceCells() {
        for (auto& [scene, cells] : m_cells) {
            for (auto& [cell_id, cell] : cells) {
                if (cell->getLoad() > CELL_MIGRATION_THRESHOLD) {
                    // 将此Cell迁移到负载较低的GameNode
                    migrateCellToNode(scene, cell_id, selectLowLoadNode());
                }
            }
        }
    }

private:
    static constexpr float CELL_SIZE = 1000.0f;  // 1km × 1km
    static constexpr float CELL_MIGRATION_THRESHOLD = 0.90f;
    
    // scene_id → cell_id → SceneThread
    std::unordered_map<uint32_t, 
        std::unordered_map<uint32_t, std::unique_ptr<SceneThread>>> m_cells;
    std::unordered_map<uint64_t, uint32_t> m_player_cell;
    
    AOIBorderManager m_aoi_border;  // Cell边界AOI管理
};
```

### 5.4 Cell 边界 AOI

```cpp
// [PRODUCTION] Cell边界AOI — 跨Cell玩家可见性
class AOIBorderManager {
public:
    // 玩家在Cell边界附近时，需要看到相邻Cell的玩家
    void updateBorderVisibility(uint32_t from_cell, uint32_t to_cell,
                                 uint64_t player_id) {
        // 1. 获取相邻Cell列表
        auto neighbors = getNeighborCells(to_cell);
        
        // 2. 在相邻Cell注册"影子观察者"
        for (uint32_t neighbor : neighbors) {
            m_shadow_observers[neighbor].insert(player_id);
        }
        
        // 3. 相邻Cell的AOI更新也发给此玩家
        // 通过进程内消息队列跨Cell传递
    }

    // 相邻Cell间AOI消息传递（批量）
    void sendBorderUpdates(uint32_t cell_id, 
                           const std::vector<AOIUpdate>& updates) {
        auto it = m_shadow_observers.find(cell_id);
        if (it == m_shadow_observers.end()) return;
        
        // 批量发送给所有影子观察者
        for (uint64_t observer : it->second) {
            m_ipc.sendToSceneThread(getPlayerCell(observer), 
                serializeAOIUpdates(updates, observer));
        }
    }

private:
    std::unordered_map<uint32_t, std::unordered_set<uint64_t>> m_shadow_observers;
};
```

---

## 6. AOI 空间优化与跨节点协调

### 6.1 预测性预取

```cpp
// [PRODUCTION] AOI预测性预取 — 根据移动方向预计算视野变化
class AOIPredictivePrefetch {
public:
    // 每帧分析玩家移动趋势
    void tick() {
        for (auto& [player_id, tracker] : m_trackers) {
            // 1. 计算移动方向和速度
            Vector3 velocity = tracker.current_pos - tracker.last_pos;
            float speed = velocity.length();
            
            if (speed < 0.1f) continue;  // 静止玩家不预取
            
            // 2. 预测下一秒位置
            Vector3 predicted_pos = tracker.current_pos + velocity * PREFETCH_TIME;
            
            // 3. 找出即将进入视野的实体
            auto will_enter = m_grid.queryRange(predicted_pos, VIEW_RANGE);
            auto currently_visible = m_aoi.getVisibleSet(player_id);
            
            // 4. 差集 = 即将新进入视野的实体
            std::vector<uint64_t> new_entities;
            std::set_difference(will_enter.begin(), will_enter.end(),
                               currently_visible.begin(), currently_visible.end(),
                               std::back_inserter(new_entities));
            
            // 5. 预取新实体的数据到本地缓存
            for (uint64_t entity : new_entities) {
                if (m_l1_cache.get(entity) == nullptr) {
                    // 从Redis预取
                    m_prefetch_queue.push(entity);
                }
            }
        }
    }

private:
    static constexpr float PREFETCH_TIME = 1.0f;  // 预测1秒后
    
    struct PlayerTracker {
        Vector3 current_pos;
        Vector3 last_pos;
        int64_t last_update;
    };
    
    std::unordered_map<uint64_t, PlayerTracker> m_trackers;
    std::queue<uint64_t> m_prefetch_queue;
};
```

### 6.2 增量差分压缩

```cpp
// [PRODUCTION] AOI增量差分 — 仅发送变化字段
class AOIDeltaCompressor {
public:
    // 计算两次更新之间的差异
    DeltaUpdate computeDelta(uint64_t entity_id, 
                              const AttributeUpdate& current,
                              const AttributeUpdate& last_sent) {
        DeltaUpdate delta;
        delta.entity_id = entity_id;
        
        // 位置变化（增量编码）
        if (current.position != last_sent.position) {
            delta.has_position = true;
            delta.pos_delta = current.position - last_sent.position;
        }
        
        // HP变化
        if (current.hp != last_sent.hp) {
            delta.has_hp = true;
            delta.hp_delta = current.hp - last_sent.hp;
        }
        
        // 朝向变化
        if (current.facing != last_sent.facing) {
            delta.has_facing = true;
            delta.facing = current.facing;
        }
        
        // Buff变化（位掩码差异）
        if (current.buff_flags != last_sent.buff_flags) {
            delta.has_buff = true;
            delta.buff_added = current.buff_flags & ~last_sent.buff_flags;
            delta.buff_removed = last_sent.buff_flags & ~current.buff_flags;
        }
        
        // 动画/状态变化（仅状态变化时发送）
        if (current.combat_state != last_sent.combat_state) {
            delta.has_state = true;
            delta.combat_state = current.combat_state;
        }
        
        return delta;
    }

    // 编码增量消息（极紧凑）
    BitBuffer encode(const DeltaUpdate& delta) {
        BitBuffer buf;
        
        // 变化位掩码（1 byte: 哪些字段有变化）
        uint8_t change_mask = 0;
        if (delta.has_position) change_mask |= 0x01;
        if (delta.has_hp)      change_mask |= 0x02;
        if (delta.has_facing)  change_mask |= 0x04;
        if (delta.has_buff)    change_mask |= 0x08;
        if (delta.has_state)   change_mask |= 0x10;
        buf.writeBits(change_mask, 5);
        
        // 仅编码有变化的字段
        if (delta.has_position) {
            encodeDeltaPosition(buf, delta.pos_delta);  // 变长增量编码
        }
        if (delta.has_hp) {
            buf.writeBits(delta.hp_delta + 32767, 16);  // 16-bit有符号
        }
        if (delta.has_facing) {
            buf.writeBits(delta.facing, 8);
        }
        if (delta.has_buff) {
            buf.writeBits(delta.buff_added, 16);
            buf.writeBits(delta.buff_removed, 16);
        }
        if (delta.has_state) {
            buf.writeBits(delta.combat_state, 4);
        }
        
        return buf;
    }

private:
    // 记录每个观察者上次收到的每个实体状态
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, AttributeUpdate>> 
        m_last_sent;
};
```

### 6.3 层次化网格

```
传统单一网格:
  网格大小: 50m × 50m
  1000m × 1000m场景 = 400个网格
  200m视野 → 查询16个网格

层次化网格:
  Level 0 (细网格): 25m × 25m  → 近距查询(LOD0, 30m)
  Level 1 (中网格): 100m × 100m → 中距查询(LOD1, 80m) 
  Level 2 (粗网格): 400m × 400m → 远距查询(LOD2, 200m)

  查询效率:
  - LOD0: 查询9个L0网格 (75m × 75m范围)
  - LOD1: 查询4个L1网格 (200m × 200m范围)
  - LOD2: 查询1个L2网格 (400m × 400m范围)
  
  vs 传统: 每次查询16个网格 → 层次化平均查询7个网格
```

```cpp
// [PRODUCTION] 层次化网格 — 多级空间索引
class HierarchicalGrid {
public:
    void insert(uint64_t entity_id, const Vector3& pos) {
        // 同时插入所有层级
        m_grid_l0[cellL0(pos)].insert(entity_id);
        m_grid_l1[cellL1(pos)].insert(entity_id);
        m_grid_l2[cellL2(pos)].insert(entity_id);
    }

    // 按LOD级别查询（不同精度）
    std::vector<uint64_t> query(const Vector3& center, float radius) {
        if (radius <= 30.0f) {
            // LOD0: 查询细网格
            return queryGrid(m_grid_l0, center, radius);
        } else if (radius <= 80.0f) {
            // LOD1: 查询中网格
            return queryGrid(m_grid_l1, center, radius);
        } else {
            // LOD2: 查询粗网格
            return queryGrid(m_grid_l2, center, radius);
        }
    }

private:
    static constexpr float L0_CELL = 25.0f;   // 25m
    static constexpr float L1_CELL = 100.0f;  // 100m
    static constexpr float L2_CELL = 400.0f;  // 400m
    
    std::unordered_map<GridCell, std::unordered_set<uint64_t>> m_grid_l0;
    std::unordered_map<GridCell, std::unordered_set<uint64_t>> m_grid_l1;
    std::unordered_map<GridCell, std::unordered_set<uint64_t>> m_grid_l2;
};
```

### 6.4 跨节点 AOI 聚合

```cpp
// [PRODUCTION] 跨节点AOI聚合 — 减少Redis Pub/Sub消息量
class CrossNodeAOIAggregator {
public:
    // 本节点的玩家位置变更（不逐条发Pub/Sub，聚合后批量发）
    void onLocalPositionChange(uint64_t player_id, const Vector3& pos) {
        m_pending_updates.push_back({player_id, pos, now()});
    }

    // 每100ms聚合发送一次（而非每帧每玩家一条）
    void flushAggregated() {
        if (m_pending_updates.empty()) return;
        
        // 1. 序列化为批量消息
        auto batch = serializeBatch(m_pending_updates);
        
        // 2. 单条Redis Pub/Sub发布（而非N条）
        m_redis.publish("cross_aoi:" + sceneChannel(m_scene_id), batch);
        
        // 3. 清空缓冲区
        m_pending_updates.clear();
        
        m_publish_count++;
    }

    // 接收其他节点的聚合消息
    void onRemoteBatch(const std::vector<RemotePositionUpdate>& updates) {
        for (const auto& update : updates) {
            // 更新远程实体影子位置
            m_shadow_entities[update.player_id] = update;
        }
    }

    // 统计: 消息量对比
    void printStats() {
        LOG_INFO("Cross-node AOI: {} entities, {} batch publishes (vs {} individual)",
                 m_shadow_entities.size(), m_publish_count,
                 m_publish_count * 30);  // 假设每batch含30个实体
    }

private:
    static constexpr int64_t FLUSH_INTERVAL_MS = 100;
    
    struct PositionUpdate {
        uint64_t player_id;
        Vector3 position;
        int64_t timestamp;
    };
    
    struct RemotePositionUpdate {
        uint64_t player_id;
        Vector3 position;
        uint32_t source_node;
    };
    
    std::vector<PositionUpdate> m_pending_updates;
    std::unordered_map<uint64_t, RemotePositionUpdate> m_shadow_entities;
    uint64_t m_publish_count{0};
};
```

**消息量对比**：

| 场景 | v2.0 (逐条Pub/Sub) | v3.0 (聚合) | 减少 |
|------|-------------------|-----------|------|
| 1000人跨节点AOI | 1000条/s × 30fps = 30,000条/s | 1条/100ms = 10条/s | **-99.97%** |
| Redis Pub/Sub带宽 | ~3MB/s | ~10KB/s | **-99.7%** |

---

## 7. v3.0 架构变更总览

### 7.1 新增 ADR

#### ADR-011: 共享内存IPC替代gRPC（同节点）

**决策**: 同K8s节点的Gateway↔GameNode通信使用共享内存RingBuffer替代gRPC。

**理由**: gRPC的序列化+HTTP/2+TCP开销在5万并发下成为主要延迟来源（500+连接）。共享内存零拷贝、零序列化，延迟从1ms降至0.02ms。

**代价**: 要求Gateway和GameNode部署在同一K8s节点（亲和性约束），共享内存管理复杂（崩溃恢复、内存泄漏检测）。

#### ADR-012: 场景级线程隔离 + Job System

**决策**: GameNode从单线程主循环改为场景级线程隔离（每场景独立线程），非关键任务通过Job System并行化。

**理由**: 单线程下8核CPU利用率仅12.5%。场景级线程隔离让不同场景的AOI/战斗/寻路并行执行，CPU利用率提升至75%。

**代价**: 跨场景通信需要无锁队列，玩家跨场景迁移需要状态序列化/反序列化。

#### ADR-013: 三级缓存 + Redis直连 + WAL

**决策**: L1进程内缓存 → L2 Redis直连 → L3 MySQL。写入使用WAL保证持久性。

**理由**: 所有DB读写经DataService代理导致2跳延迟和单点瓶颈。L1缓存命中率>95%时几乎消除Redis往返。WAL替代30s批量flush实现零数据丢失。

**代价**: L1缓存需要内存（~2GB/节点），缓存一致性需要版本号校验。WAL需要额外磁盘空间。

#### ADR-014: QUIC协议 + 边缘网关

**决策**: 客户端连接使用QUIC替代TCP，部署边缘网关就近接入。

**理由**: TCP 3次握手100ms首包延迟影响体验。QUIC 0-RTT消除重连延迟，连接迁移支持移动端网络切换。边缘网关降低玩家到服务器RTT 80%。

**代价**: QUIC实现复杂度高于TCP，边缘网关需要多地部署（增加运维成本）。

#### ADR-015: 多级背压 + 自适应降级

**决策**: 4级背压（GREEN/YELLOW/ORANGE/RED）渐进式功能裁剪，替代85%硬性降级。

**理由**: 硬性降级在85%阈值处体验突变。4级渐进式降级让非关键功能逐步退出，核心体验尽可能保持。

**代价**: 降级策略配置复杂，需要精细的feature分级和恢复逻辑。

#### ADR-016: Cell-based开放世界

**决策**: 大型开放世界场景分Cell管理，每Cell独立线程，支持Cell间迁移和负载均衡。

**理由**: 传统一个场景一个线程的模型在大场景(5000m+)下无法支撑高人数。Cell分片让1000人场景分散到25个Cell线程，每线程仅40人。

**代价**: Cell边界AOI需要额外管理，跨Cell迁移有开销（~5ms状态序列化）。

### 7.2 更新后的性能红线

| 指标 | v2.0 红线 | v3.0 红线 | 变更 |
|------|----------|----------|------|
| 战斗循环(1000人) | <=5ms | <=5ms | 不变 |
| 单节点消息吞吐 | <=8000条/s | **<=20000条/s** | **+150%** |
| Gateway↔GameNode延迟 | <1ms | **<0.05ms** | **-95%** |
| 热数据读取延迟 | <1ms (Redis) | **<0.01ms (L1)** | **-99%** |
| 数据写入延迟 | <3ms | **<0.1ms (WAL)** | **-97%** |
| 客户端重连延迟 | ~100ms | **~0ms (0-RTT)** | **-100%** |
| L1缓存命中率 | - | **>=95%** | 新增 |
| CPU利用率 | - | **>=70%** | 新增 |
| 跨节点AOI消息量 | - | **<=10条/s** | 新增 |

### 7.3 更新后的容量规划

| 资源 | v2.0 | v3.0 | 变更 |
|------|------|------|------|
| Gateway | 10+ (4C8G) | 10+ 边缘 (2C4G) + 10+ 中心 (4C8G) | 分层 |
| GameNode | 50+ (8C16G) | 50+ (8C16G) | 不变(但吞吐2.5x) |
| DataService | 8+ (4C8G) | **4+ (4C8G)** | **-50%** (L1缓存+直连) |
| Redis分片 | 16 (8C32G) | 16 (8C32G) | 不变 |
| MySQL | 8主 + 0从 | 8主 + **8从** | **+读副本** |
| 共享内存 | - | 50×1GB = **50GB** | 新增 |
| WAL存储 | - | 50×50MB = **2.5GB** | 新增 |

### 7.4 综合性能提升总览

```
v2.0 → v3.0 关键指标提升:

  通信延迟:     1ms ────────────────────→ 0.02ms     (-98%)
  节点吞吐:     8,000条/s ─────────────→ 20,000条/s  (+150%)
  热数据读取:   3ms ────────────────────→ 0.01ms     (-99.7%)
  数据写入:     3ms ────────────────────→ 0.1ms      (-97%)
  重连延迟:     100ms ──────────────────→ 0ms        (-100%)
  CPU利用:      12.5% ──────────────────→ 75%        (+500%)
  跨节点AOI:    30,000条/s ─────────────→ 10条/s     (-99.97%)
  过载保护:     硬性85% ────────────────→ 渐进4级     (质变)
  
  5万在线集群总量:
    消息处理: 400,000条/s → 1,000,000条/s
    Redis QPS: 500,000 → 50,000 (L1吸收90%)
    MySQL写入QPS: 50,000 → 10,000 (WAL批量)
```

---

## 8. 实施计划

### 8.1 分阶段实施

```
Phase 1 (Week 3): 通信层优化
  ├─ 共享内存IPC实现 + K8s亲和性部署
  ├─ Redis直连客户端 + L1缓存
  └─ 批量gRPC（跨节点）

Phase 2 (Week 4): GameNode并发
  ├─ 场景线程模型重构
  ├─ Job System实现
  ├─ ECS数据布局迁移
  └─ 无锁队列

Phase 3 (Week 5): 数据层 + 网络
  ├─ WAL + Write-Behind
  ├─ MySQL读写分离
  ├─ QUIC传输层
  └─ 边缘网关部署

Phase 4 (Week 6): 负载管理 + AOI
  ├─ 背压系统 + 多级降级
  ├─ Cell-based场景管理
  ├─ AOI预测预取 + 差分压缩
  └─ 跨节点AOI聚合

Phase 5 (Week 7): 集成验证
  ├─ 全模块集成测试
  ├─ 5万并发压测
  └─ 性能调优 + 故障演练
```

### 8.2 验收标准

| # | 验收项 | 验收标准 |
|---|--------|---------|
| 1 | IPC延迟 | 同节点消息延迟 <=0.05ms |
| 2 | 节点吞吐 | 单节点有效消息 >=20,000条/s |
| 3 | L1缓存命中 | 热数据命中率 >=95% |
| 4 | CPU利用 | 单节点CPU利用率 >=70% |
| 5 | QUIC重连 | 0-RTT重连成功率 >=99% |
| 6 | 背压响应 | GREEN→RED切换 <=100ms |
| 7 | Cell迁移 | 跨Cell迁移 <=5ms |
| 8 | 跨节点AOI | Pub/Sub消息量 <=10条/s |

---

> **文档维护**: 本文档为CAMI v3.0高并发优化的核心设计文档。v2.0的6个优化领域（客户端预测/寻路/碰撞/AOI LOD/法术批次/位压缩）仍然有效，v3.0在此基础上进一步优化通信延迟和服务器压力。
