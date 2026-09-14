# TASK-036 · Resource / Low Spec System — Public Interface

All modules live under `client/resource/`. They are **independent modules** (not processes),
config-driven (no hard-coded quality params — spec §21 Forbidden), and consume/present data
through explicit interfaces only. The renderer reads `QualityPreset` to tune LOD/draw; the
streamer owns chunk lifecycle; `ResourceManager` owns budgeted async load + LRU reclaim.

## QualityPreset (`quality_preset.gd`)
Three presets (Low/Medium/High) loaded from `config/client/quality.json`. No global `class_name`
(godot `--script` parse scope does not resolve global class names; callers use
`preload("res://resource/quality_preset.gd")`).

```
load_config(cfg: Dictionary, level: int) -> void   # apply parsed json for level 0/1/2
static load_file(path: String, level: int) -> Resource
ground_grid() -> int                                 # cells per chunk side: 16/24/32
name() -> String                                     # "low"|"medium"|"high"
```
Fields: `level, texture_cap, ground_density_level, lod_aggressive, view_dist, chunk_radius,
texture_budget_mb, mesh_budget_mb, triangle_budget, atlas_res, same_screen_entities, particles,
net_rate, shadow`.

## ResourceManager (`resource_manager.gd`) — extends Node
Async load / refcount / per-type LRU reclaim / quality hot-swap. Memory is **accounted** by
type+budget in headless (no GPU raster available); `ResHandle` (RefCounted) stands in for the
loaded asset, so the public interface is identical to a real `ResourceLoader` path.

```
init(preset) -> void
load_from_config_file(path: String, level: int) -> void
load_async(uri: String, type: int) -> RefCounted      # type: TYPE_TEXTURE=0 / TYPE_MESH=1 / TYPE_OTHER=2
unload(res: RefCounted) -> void
set_quality(level: int) -> void                        # re-apply preset, reclaim over-budget
stats() -> Dictionary                                  # ram_mb, vram_mb, texture_mem_mb, mesh_mem_mb, draw_calls, loaded, pending, last_load_ms
```
LRU eviction only drops entries with `refs == 0` (live refs are never freed).

## SceneStreamer (`scene_streamer.gd`) — extends Node
3D world chunk streaming. Current chunk + radius neighborhood loaded; beyond `radius+HYSTERESIS`
→ `unloading` phase; unloaded after `UNLOAD_DELAY_SEC=5s`. Per-frame budget (`MAX_NEW_PER_FRAME=1`,
`MAX_COMPLETE_PER_FRAME=1`) caps load spikes (spec §9/§15.6-7). Hysteresis prevents flapping when
the player crosses a boundary back and forth.

```
init(preset) -> void
set_preset(preset) -> void
update(player_pos: Vector3, dt := 1/60) -> void        # main-thread per-frame
force_unload_all() -> void
stats() -> Dictionary                                  # loaded_chunks, pending, last_load_ms, load_ms_p95, load_count, unload_count
```
Constants: `CHUNK_SIZE=128`, `UNLOAD_DELAY_SEC=5.0`, `HYSTERESIS=1`.

## ProcMeshCache (`proc_mesh_cache.gd`) — extends Node
Procedural ground mesh cache (no DCC models — spec §21). One `ArrayMesh` per chunk key; cache hit
reuses the mesh instead of rebuilding.

```
set_lod_density(level: int) -> void                   # 0/1/2 -> grid 16/24/32
get_or_build(chunk_key: Vector2i, hm: Image) -> ArrayMesh   # hm may be null (flat build headless)
chunk_triangle_count() -> int
hit_rate() -> float
cache_size() -> int
```

## Headless constraint (honesty)
Godot headless cannot instantiate `Image`/`Texture` rasters (`Image.create`/`create_from_data`
return 0×0 — confirmed via `_img_probe.gd` in TASK-035). Therefore: heightmaps use a flat build,
resource memory is **budget-accounted** (not GPU-measured), and load latency uses real
`Time.get_ticks_usec()` deltas. On a real device the same code path loads via `ResourceLoader`
and the byte model is replaced by actual VRAM — the public interface is unchanged.
