# Inventory / Equipment — 性能（TASK-017 §18 / §22）

## 预算（§22）
| 项 | 预算 | 实测（Release, --players 1000 --ops 100） |
|---|---|---|
| Add | < 200ns | 见 bench/inventory.txt `add_ns` |
| Remove | < 200ns | `remove_ns` |
| Equip（含重算） | < 2us | `equip_ns` |
| 单物品内存 | < 64B | `mem_bytes_per_item`（= sizeof(ItemStack)=24B） |
| 单玩家背包 | < 8KB | 100×24B=2.4KB 实例 + 10×24B 装备 = 2.64KB |

## 计时口径
Windows `steady_clock` 分辨率约 100ns，单次操作（几十 ns）无法直接采样 → 批量计时 + 除法：
每轮遍历全部 N 玩家各操作一次，累计 R 轮取 `总时间/(N*R)` 中位数（同 role_bench）。

## 内存口径
`ItemStack` = 8(def_id)+4(count)+4(durability)+8(guid) = 24B（§22 < 64B）。物品实例存于玩家背包
定长数组槽，无逐物品堆分配；装备栏独立 10 槽。验收断言 `mem_bytes_per_item ≤ 64`。

## 实测（粘贴自验收脚本真实输出）
见 `bench/inventory.txt`，典型值：
```
players=1000
add_ns=...        remove_ns=...
equip_ns=...      recompute_after_equip_ns=...
mem_bytes_per_item=24.000
```
`equip_ns ≤ 2000` 与 `mem_bytes_per_item ≤ 64` 为验收硬阈值（task-017.sh 断言）。
