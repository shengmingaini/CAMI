# TASK-035 · Renderer (2.5D) — Performance Report

Benchmark scene per spec §17: **1000 ground cells (32×32 grid) + 200 sprite entities
+ 50 UI nodes**, run for 600 frames at 60 Hz. Metrics are scene-graph derived
(headless has no GPU readback); counts reflect what the renderer actually instantiates.

## Measured results (`bench/render_<quality>.txt`)

| Metric | Low | Medium | High | Threshold |
|---|---|---|---|---|
| `draw_calls` | 202 | 202 | 202 | < 300 |
| `triangles` | 2422 | 2422 | 2422 | < 300 000 |
| `texture_mem_mb` | 1.0 | 4.0 | 16.0 | < 512 |
| `cpu_ms` (p95/frame) | 0.065 | 0.063 | 0.065 | — |
| `fps_p95` (derived 1000/cpu_ms) | 15 385 | 15 873 | 15 385 | — |
| `shader_switches` | 3 | 3 | 3 | — |
| `visible_sprites` | 200 | 200 | 200 | — |
| `verdict_pass` | true | true | true | true |

## Why the numbers are honest

- **One ground mesh = one draw call.** `ProcGround` commits a single indexed `ArrayMesh`
  for the whole grid, so the "1000 ground blocks" cost 1 draw call regardless of cell count
  (triangle count read back from the real index buffer: 31×31×2 = 1922 for 32×32).
- **Sprite draw calls** = one `Sprite3D` each (200). A production build would batch these
  via a `MultiMesh`/`TextureAtlas` to collapse toward a single draw call — the architecture
  already isolates this behind `SpriteEntity`, so the optimization is local and non-breaking.
- **`texture_mem_mb`** is the atlas-resolution budget (512²/1024²/2048² × 4 bytes), the
  dominant GPU texture cost; it is well under the 512 MB Low ceiling.
- **`cpu_ms`** is measured client CPU work per frame (cull + LOD + camera follow), not a
  theoretical value. The 60 Hz design budget is 16.6 ms; measured ≈ 0.06 ms leaves a large
  headroom for gameplay logic on the same tick.
- **`gpu_ms`** is 0 in headless (no rasterizer); on a real device it is bounded by the
  draw-call / triangle / texture budgets above, all within spec.

## Low-spec guarantees (spec §35)

Low preset targets 4-core CPU / 4 GB RAM / 1 GB VRAM / DX11. The measured texture budget
(1 MB atlas) and draw-call count (202) are trivially within that envelope; LOD + distance
cull keep far sprites off both the draw list and the triangle list. Actual FPS on the
target hardware must still be confirmed by a device benchmark before claiming compatibility.
