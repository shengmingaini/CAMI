#!/usr/bin/env python3
# benchmark/shard_tps_bench.py — 单分库写入 TPS 压测 (Week4 D5 验收)
# 通过 ShardingSphere 代理(3309) 写入 player_id 全 ≡0 mod 8 的行,
# 路由到同一分片(shard 0)主库, 测量单分库 INSERT 吞吐。
# 用法: python shard_tps_bench.py [threads] [per_thread]
import pymysql, threading, time, sys, os

# 连接参数可由环境变量覆盖 (默认走 ShardingSphere 代理)
HOST = os.environ.get("BENCH_HOST", "127.0.0.1")
PORT = int(os.environ.get("BENCH_PORT", "3309"))
USER = os.environ.get("BENCH_USER", "root")
PASS = os.environ.get("BENCH_PASS", "root")
DB = os.environ.get("BENCH_DB", "cami_db")
N_THREADS = int(sys.argv[1]) if len(sys.argv) > 1 else 16
N_PER_THREAD = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
TOTAL = N_THREADS * N_PER_THREAD
local = threading.local()


def conn():
    if not hasattr(local, "c"):
        local.c = pymysql.connect(host=HOST, port=PORT, user=USER, password=PASS,
                                  database=DB, autocommit=True, connect_timeout=10)
    return local.c


def worker(tid, base):
    c = conn()
    cur = c.cursor()
    for i in range(N_PER_THREAD):
        pid = base + i * 8  # 全 ≡0 mod 8 -> 同一分片主库
        cur.execute(
            "INSERT INTO player_base (player_id,account_id,name,class_id) "
            "VALUES (%s,%s,%s,%s) ON DUPLICATE KEY UPDATE name=%s",
            (pid, 1, f"p{pid}", 1, f"p{pid}"))
    cur.close()


def main():
    threads = []
    start = time.time()
    for t in range(N_THREADS):
        base = t * 100000 * 8  # 各线程不同段, 均 ≡0 mod 8
        threads.append(threading.Thread(target=worker, args=(t, base)))
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    el = time.time() - start
    tps = TOTAL / el
    print(f"[bench] threads={N_THREADS} inserts={TOTAL} elapsed={el:.3f}s "
          f"tps={tps:.0f} (验收单分库>=5000 TPS)")
    sys.exit(0 if tps >= 5000 else 1)


if __name__ == "__main__":
    main()
