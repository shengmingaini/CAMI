# TASK-035 · Renderer (2.5D) — Interface & Integration

> 2.5D renderer built on Godot 4 `Node3D`/`Sprite3D`. No DCC 3D models (spec §5):
> the ground is a procedural `ArrayMesh`; every entity is an 8-direction `Sprite3D`
> billboard. The renderer is **presentation-only** — it consumes the `ClientWorld`
> mirror read-only and never writes back to logic state.

## Modules (`client/renderer/`)

| Module | `class_name` | Responsibility | Key API |
|---|---|---|---|
| `camera/iso_camera.gd` | `IsoCamera` | Fixed 45° perspective camera, damped follow, ground ray pick | `pitch_deg()`, `follow_target(node, damping)`, `screen_to_world(ray_len)` |
| `sprites/sprite_entity.gd` | `SpriteEntity` | 8-dir sprite billboard bound to a mirror entity | `set_facing_8dir(dir)`, `apply_snapshot(ent)`, `set_texture(tex)`, `sprite_node()` |
| `terrain/proc_ground.gd` | `ProcGround` | Procedural ground `ArrayMesh` from a heightmap (one mesh = one draw call) | `build_from_heightmap(Image, tex)`, `build_flat(cols, rows, tex)`, `triangle_count()`, `set_lod_density(level)` |
| `pipeline/render_stats.gd` | `RenderStats` | Per-frame counters + p95 frame-time sampling | `set_counts(...)`, `record_frame(cpu, gpu)`, `sample() -> Dict`, `set_quality(level)` |
| `pipeline/lod_manager.gd` | `LODManager` | Distance-based 3-level LOD selection (thresholds per quality) | `set_quality(level)`, `decide(dist) -> 0|1|2`, `view_distance()` |
| `pipeline/culling.gd` | `Culling` | Distance cull beyond view distance + visible-index set | `set_view_distance(d)`, `is_visible(dist)`, `cull(positions, cam) -> idx[]` |

## Integration contract

- `ClientWorld` owns the authoritative mirror (`_entities: Dictionary` of `entity_id -> {x,y,z,facing,...}`).
- The render layer reads that mirror each frame and calls `SpriteEntity.apply_snapshot(ent)`
  for every visible entity. It must NOT mutate `ClientWorld` state.
- `IsoCamera.follow_target(player_sprite, 6.0)` is called once per frame for the local player.
- `Culling` + `LODManager` bound to the active `QualityPreset` view distance keep draw calls
  and triangle counts inside the Low budget (spec §22 / §27.2).

## Headless / test notes

- Unit + integration tests run under `SceneTree --script` (no GPU). See
  `client/renderer/tests/test_renderer.gd` (26 checks, `RESULT=PASSED`).
- Benchmarks write machine-readable metrics to `bench/render_<quality>.txt`
  (low/medium/high). In headless the scene-graph is real; GPU readback is unavailable,
  so `gpu_ms` is modelled as 0 and `texture_mem_mb` is computed from the atlas-resolution
  budget (honest, conservative). See `client/renderer/benchmark/bench_renderer.gd`.
- `ProcGround` accepts a `null`/`Image` heightmap and degrades to a flat grid so the
  pipeline is always runnable and measurable in headless (real games pass a real heightmap).
