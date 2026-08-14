# data/CMakeLists.txt — 数据管理层 (spec §3.1)
# Redis缓存代理 / MySQL分库分表代理 / 数据同步 / 版本校验 / L1缓存 / WAL。
# 依赖下层：common。
add_library(cami_data STATIC
    data_layer.cpp
    redis_proxy/cache_proxy.cpp
)
target_include_directories(cami_data PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(cami_data PUBLIC cxx_std_17)
target_link_libraries(cami_data PUBLIC cami_common)

# --- [PRODUCTION] 真实后端 (CAMI_BUILD_MODULES=ON + vcpkg) ---
# OFF 构建不编译这些 .cpp (各文件整体 #ifdef CAMI_BUILD_MODULES 守护, 为空 TU)。
if(CAMI_BUILD_MODULES)
    find_package(redis++ CONFIG REQUIRED)
    find_package(Threads REQUIRED)

    # Redis 缓存后端: 本地 MinGW + CI(Linux) 均安装, 始终编译。
    target_sources(cami_data PRIVATE
        redis_proxy/redis_backend.cpp      # Redis 缓存后端 (CacheBackend)
    )
    # PUBLIC (非 PRIVATE): RedisBackend 是 cami_data 的公开生产契约, cami_data_service_main
    # 直接实例化它, 链接依赖必须向下传递 redis++, 否则 MODULES=ON 下 main 链接报未定义符号。
    target_link_libraries(cami_data PUBLIC
        redis++::redis++_static
    )

    # --- 可选生产后端: 仅当对应 vcpkg 包已安装时编译 ---
    # 本地 MinGW 可能缺 libmysql/cppkafka/protobuf/grpc (libmysql 不支持 mingw),
    # 这些后端在 Linux CI (build-modules-on, 全量安装) 上照常编译。
    # 链接统一用 PUBLIC: 同 Redis 理由, KafkaFlush / MySQLBackingStore 由 main 直接实例化,
    # cami_data_service_main 经 cami_data_service(PUBLIC cami_data) 必须取得这些传递依赖。
    # cppkafka 上游 CMake 包名为 CppKafka (大写 C/K), target 为 CppKafka::cppkafka。
    # (find_package(cppkafka) 小写找不到 config, 会导致 Kafka 后端被静默跳过。)
    find_package(CppKafka CONFIG QUIET)
    if(CppKafka_FOUND)
        target_sources(cami_data PRIVATE sync/kafka_flush.cpp)   # Kafka 异步落库 + 死信队列
        target_link_libraries(cami_data PUBLIC CppKafka::cppkafka)
        message(STATUS "[data] Kafka 落库后端已启用 (cppkafka)")
    else()
        message(STATUS "[data] cppkafka 未安装, 跳过 Kafka 后端")
    endif()

    # libmariadb (MariaDB Connector/C) 替代 libmysql (MySQL 官方 8.0.x)。
    # 原因: vcpkg 的 libmysql port 依赖 boost×5 + ncurses + lz4 + zstd + rapidjson,
    # 构建极易失败 (已知 issue #46011), 且 MySQL 8.0 硬编码 boost_1_77_0。
    # libmariadb 是 MySQL 客户端的 drop-in 替代 (<mysql.h> API 兼容, 头文件同样
    # 装在 include/mysql/), 依赖仅 zlib+openssl, 构建稳定, 服务器端仍是 MySQL 8.0+SS。
    # vcpkg 导出包名 unofficial-libmariadb, target 名 unofficial::libmariadb。
    find_package(unofficial-libmariadb CONFIG QUIET)
    if(unofficial-libmariadb_FOUND)
        target_sources(cami_data PRIVATE mysql_proxy/mysql_backing_store.cpp)  # MySQL 落库后端
        target_link_libraries(cami_data PUBLIC unofficial::libmariadb)
        message(STATUS "[data] MySQL 落库后端已启用 (libmariadb)")
    else()
        message(STATUS "[data] libmariadb 未安装 (MinGW 不支持), 跳过 MySQL 后端")
    endif()

    # --- [阶段 A W4] RocksDB 嵌入式 KV 落库后端 (vcpkg: rocksdb) ---
    # 替代 MySQL/SS/Redis 外部栈, 单进程内嵌 KV + WAL + 事件溯源; 与 MySQL 后端并存,
    # 由 StorageRouter 做影子部署/熔断/自主晋升 (见 data/rocksdb_proxy/storage_router.h)。
    # 目标名以 vcpkg 安装输出为准 (RocksDB::rocksdb / rocksdb); CI 若改名此处同步。
    find_package(RocksDB CONFIG QUIET)
    if(RocksDB_FOUND)
        target_sources(cami_data PRIVATE rocksdb_proxy/rocksdb_backing_store.cpp)
        target_link_libraries(cami_data PUBLIC RocksDB::rocksdb)
        # RocksDB 11.x 头文件要求 C++20 (operator==()=default / using enum), 而项目其余 TU 为 C++17。
        # 仅此文件用 C++20 编译; 边界类型为标准库类型, 与 C++17 TU 链接 ABI 兼容。
        set_source_files_properties(rocksdb_proxy/rocksdb_backing_store.cpp
            PROPERTIES COMPILE_OPTIONS "-std=c++20")
        message(STATUS "[data] RocksDB 落库后端已启用 (RocksDB, C++20)")
    else()
        find_package(rocksdb CONFIG QUIET)
        if(rocksdb_FOUND)
            target_sources(cami_data PRIVATE rocksdb_proxy/rocksdb_backing_store.cpp)
            target_link_libraries(cami_data PUBLIC rocksdb)
            message(STATUS "[data] RocksDB 落库后端已启用 (rocksdb)")
        else()
            message(STATUS "[data] RocksDB 未安装, 跳过 RocksDB 后端")
        endif()
    endif()

    # gRPC Data Service (需要 protoc 代码生成, 见 data_service/CMakeLists.txt)
    find_package(Protobuf CONFIG QUIET)
    find_package(gRPC CONFIG QUIET)
    if(Protobuf_FOUND AND gRPC_FOUND)
        add_subdirectory(data_service)
    else()
        message(STATUS "[data] Protobuf/gRPC 未安装, 跳过 Data Service 后端")
    endif()
endif()

# --- D3/D4/D5: 数据层验证/基准 (CAMI_BUILD_TESTS=ON 时构建并注册 ctest) ---
# 依赖仅 STL, CAMI_BUILD_MODULES=OFF 亦可编译。
if(CAMI_BUILD_TESTS)
    # D3 缓存代理: 读穿命中率>=95% + 写回落库 + 热点识别
    add_executable(cami_cache_demo redis_proxy/cache_proxy_demo.cpp)
    target_link_libraries(cami_cache_demo PRIVATE cami_data)
    add_test(NAME cache_proxy_demo COMMAND cami_cache_demo)

    # D5 版本校验: 并发写冲突 100% 被版本号拦截
    add_executable(cami_version_demo version/version_demo.cpp)
    target_link_libraries(cami_version_demo PRIVATE cami_data)
    add_test(NAME version_demo COMMAND cami_version_demo)

    # D4 数据同步: 30s 批量落库 + 断线立即持久化 + 版本 CAS 落库
    add_executable(cami_sync_demo sync/sync_demo.cpp)
    target_link_libraries(cami_sync_demo PRIVATE cami_data)
    add_test(NAME sync_demo COMMAND cami_sync_demo)

    # 阶段 A W4: 自主存储治理器 (影子部署/熔断/自主晋升) — 纯 STL, OFF 可编译。
    # 用 InMemoryStore 仿真双端 + ControllableStore 验证, 不依赖 RocksDB。
    add_executable(cami_storage_router_test ${CMAKE_SOURCE_DIR}/tests/unit/storage_router_test.cpp)
    target_link_libraries(cami_storage_router_test PRIVATE cami_data GTest::gtest GTest::gtest_main)
    add_test(NAME storage_router_test COMMAND cami_storage_router_test)
endif()
