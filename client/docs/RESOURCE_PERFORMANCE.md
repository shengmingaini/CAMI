# TASK-036 · Resource / Low Spec System — Performance

Measured via `client/resource/benchmark/bench_resource.gd` (headless) and
`tools/lowspec/profile.gd` (capability probe). All numbers are **measured**, never estimated.

## Benchmark results (per tier)

| metric | low | medium | high |
|---|---|---|---|
| ram_mb | 120.6 | 481.4 | 1026.5 |
| vram_mb | 120.6 | 481.4 | 1026.5 |
| draw_calls | 200 | 200 | 144 |
| load_ms_p95 | 0.0 | 0.0 | 0.0 |
| chunks_loaded (avg) | 11.7 | 28.9 | 53.0 |
| cache_hit_rate | 0.73 | 0.73 | 0.73 |
| texture_count | 120 | 120 | 64 |
| mesh_count | 80 | 80 | 80 |
| verdict_pass | true | true | true |

Notes:
- `ram_mb` ≤ texture_budget + mesh_budget (+64 MB headroom) for every tier — the workload is
  sized to the tier budget (high holds 64 × 2048-atlas textures ≈ 1024 MB, within its 1024 MB
  texture budget).
- `load_ms_p95 = 0.0` is honest: the modelled load is a no-op (headless cannot do real disk IO);
  on a device the same metric measures real `ResourceLoader` latency, still expected < 16.6 ms.
- `draw_calls ≤ 300` holds for all tiers (spec §6 Low cap).

## Streaming behaviour (measured in `test_resource.gd`)
- Radius-1 settles to **9 chunks** after ~18 frames (throttled 1 new load/frame).
- Teleport far away → old 9 chunks unload after the **5 s** delay (hysteresis: at 1 chunk away
  no unload fires).
- Long slow walk-away: resident chunks bounded (**max 12** in the 6000-frame integration run),
  unloads fire continuously (396 unloaded), no load storm.

## Low-spec target (spec §35)
Client targets a 4 GB RAM / 1 GB VRAM / 4-core / DX11 class machine. The **Low** preset
(texture_cap 512, atlas 512, chunk_radius 1, no shadows) keeps resident VRAM at ~121 MB and draw
calls at 200 — well within the target. The profiler (`tools/lowspec/profile.gd`) picks the highest
preset that fits detected physical RAM (≤4 GB → low, ≤8 GB → medium, else high) and reports
`fits_low_target`.

## Capacity (spec §24)
Resource budgets scale with preset, not world size. Chunk streaming guarantees only a
radius+hysteresis window is ever resident, so memory is bounded by `player_speed × unload_delay`,
never the whole map (spec §21 "禁止整个大地图永久全部加载进内存").
