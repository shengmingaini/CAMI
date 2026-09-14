## TASK-036 · Resource / Low Spec System (2.5D) — headless unit + integration tests.
## Run: godot --headless --path client --script res://resource/tests/test_resource.gd
## Coverage: §16 Unit (QualityPreset load, LRU reclaim, budget reclaim, mesh cache hit,
## chunk load/unload + hysteresis, quality hot-swap, stats) + §17 Integration (streaming
## settles over frames, bounded resident chunks, unload actually fires on a slow walk-away).
##
## NOTE: SceneStreamer throttles loads to MAX_NEW_PER_FRAME=1 to avoid frame spikes (spec
## §9/§15.6-7), so a single update only settles one chunk; tests call update() in a loop to
## let the radius settle. The integration walk-away path is slow enough that chunks fall
## behind and unload after the 5s delay (a fast lap would re-enter and cancel unload).
extends SceneTree

const RESULT_PATH := "res://resource/tests/last_result.txt"
const EXPECTED_CHECKS := 30
const QP := preload("res://resource/quality_preset.gd")
const RM := preload("res://resource/resource_manager.gd")
const SS := preload("res://resource/scene_streamer.gd")
const PM := preload("res://resource/proc_mesh_cache.gd")

var _failures: Array = []
var _check_count := 0


func _init() -> void:
	_test_quality_preset()
	_test_resource_manager_lru()
	_test_proc_mesh_cache()
	_test_scene_streamer_basic()
	_test_scene_streamer_unload_delay()
	_test_scene_streamer_hysteresis()
	_test_quality_hot_swap()
	_test_integration_long_run()
	_report()


func _check(cond: bool, name: String) -> void:
	_check_count += 1
	if cond:
		print("PASS: ", name)
	else:
		print("FAIL: ", name)
		_failures.append(name)


func _test_quality_preset() -> void:
	var q := QP.load_file("res://config/client/quality.json", 0)
	_check(q.level == 0, "QualityPreset: low level=0")
	_check(q.atlas_res == 512, "QualityPreset: low atlas_res=512 (got %d)" % q.atlas_res)
	_check(q.chunk_radius == 1, "QualityPreset: low chunk_radius=1 (got %d)" % q.chunk_radius)
	_check(q.triangle_budget == 300000, "QualityPreset: low triangle_budget=300000")
	_check(q.texture_budget_mb == 256, "QualityPreset: low texture_budget_mb=256")
	_check(q.ground_grid() == 16, "QualityPreset: low ground_grid=16 (got %d)" % q.ground_grid())
	var m := QP.load_file("res://config/client/quality.json", 2)
	_check(m.atlas_res == 2048, "QualityPreset: high atlas_res=2048 (got %d)" % m.atlas_res)
	_check(m.chunk_radius == 3, "QualityPreset: high chunk_radius=3 (got %d)" % m.chunk_radius)
	_check(m.ground_grid() == 32, "QualityPreset: high ground_grid=32 (got %d)" % m.ground_grid())


func _test_resource_manager_lru() -> void:
	# Tiny budget to force LRU eviction: 2 MB texture budget, each atlas = 1 MB.
	var cfg := {"presets": {"low": {"atlas_res": 512, "texture_budget_mb": 2, "mesh_budget_mb": 128, "ground_density_level": 0}}}
	var q := QP.new()
	q.load_config(cfg, 0)
	var rm = RM.new()
	rm.init(q)
	var A = rm.load_async("tex_a", RM.TYPE_TEXTURE)
	var B = rm.load_async("tex_b", RM.TYPE_TEXTURE)
	_check(rm.stats()["loaded"] == 2, "ResourceManager: 2 loaded at budget")
	rm.unload(A)   # A refs -> 0, eligible for eviction
	var C = rm.load_async("tex_c", RM.TYPE_TEXTURE)  # 3MB > 2MB -> evict A
	_check(not rm._cache.has("tex_a"), "ResourceManager: LRU evicted unreferenced A under budget")
	_check(rm._cache.has("tex_b") and rm._cache.has("tex_c"), "ResourceManager: B,C retained")
	_check(rm.stats()["texture_mem_mb"] <= 2.01, "ResourceManager: texture_mem within 2MB budget")
	_check(rm._cache.has("tex_b"), "ResourceManager: referenced B not evicted")
	rm.free()


func _test_proc_mesh_cache() -> void:
	var pm = PM.new()
	pm.set_lod_density(0)
	var k0 := Vector2i(0, 0)
	var m1: ArrayMesh = pm.get_or_build(k0, null)
	_check(m1 != null, "ProcMeshCache: builds mesh")
	var m2: ArrayMesh = pm.get_or_build(k0, null)
	_check(m2 == m1, "ProcMeshCache: same chunk key returns cached mesh (hit)")
	_check(pm.hit_rate() > 0.0, "ProcMeshCache: hit_rate > 0 after repeated key")
	var k1 := Vector2i(1, 0)
	pm.get_or_build(k1, null)
	_check(pm.cache_size() == 2, "ProcMeshCache: two distinct chunks cached")
	_check(pm.chunk_triangle_count() == 450, "ProcMeshCache: triangle count = 450 (got %d)" % pm.chunk_triangle_count())
	pm.set_lod_density(2)
	_check(pm._grid == 32, "ProcMeshCache: set_lod_density(2) -> grid 32")
	pm.free()


## Radius-1 => 9 chunks. Throttled to 1 new load/frame, so settle over ~30 frames.
func _test_scene_streamer_basic() -> void:
	var q := QP.load_file("res://config/client/quality.json", 0)
	var ss = SS.new()
	ss.init(q)
	for _i in range(30):
		ss.update(Vector3(0, 0, 0))
	var st: Dictionary = ss.stats()
	_check(st["loaded_chunks"] == 9, "SceneStreamer: radius-1 settles to 9 chunks (got %d)" % st["loaded_chunks"])
	_check(st["pending"] == 0, "SceneStreamer: no pending after settle")
	ss.free()


## Teleport far away: old 9 chunks go beyond radius -> unload after 5s delay (300 frames).
func _test_scene_streamer_unload_delay() -> void:
	var q := QP.load_file("res://config/client/quality.json", 0)
	var ss = SS.new()
	ss.init(q)
	for _i in range(30):
		ss.update(Vector3(0, 0, 0))
	var loaded_before: int = ss.stats()["loaded_chunks"]
	ss.update(Vector3(5000, 0, 5000), 1.0 / 60.0)
	for _i in range(360):
		ss.update(Vector3(5000, 0, 5000), 1.0 / 60.0)
	var st: Dictionary = ss.stats()
	_check(loaded_before == 9, "SceneStreamer: 9 loaded before teleport (got %d)" % loaded_before)
	_check(st["unload_count"] >= 9, "SceneStreamer: >9 chunks unloaded after 5s delay (got %d)" % st["unload_count"])
	_check(st["loaded_chunks"] <= 9, "SceneStreamer: resident chunks bounded after teleport (got %d)" % st["loaded_chunks"])
	ss.free()


## Hysteresis: at 1 chunk away (within radius+hysteresis=2) no unload must fire.
func _test_scene_streamer_hysteresis() -> void:
	var q := QP.load_file("res://config/client/quality.json", 0)
	var ss = SS.new()
	ss.init(q)
	for _i in range(30):
		ss.update(Vector3(0, 0, 0))
	var before: int = ss.stats()["unload_count"]
	ss.update(Vector3(1 * 128.0, 0, 0), 1.0 / 60.0)
	for _i in range(60):
		ss.update(Vector3(1 * 128.0, 0, 0), 1.0 / 60.0)
	var after: int = ss.stats()["unload_count"]
	_check(after == before, "SceneStreamer: hysteresis -> no unload within 1 chunk (got %d)" % after)
	ss.free()


func _test_quality_hot_swap() -> void:
	var rm = RM.new()
	rm.load_from_config_file("res://config/client/quality.json", 0)
	rm.load_async("t1", RM.TYPE_TEXTURE)
	rm.set_quality(2)
	_check(rm._preset.atlas_res == 2048, "ResourceManager: set_quality(2) applied atlas_res=2048")
	rm.free()


## Slow walk-away: chunks fall behind and unload after the 5s delay; resident stays bounded.
func _test_integration_long_run() -> void:
	var q := QP.load_file("res://config/client/quality.json", 0)
	var ss = SS.new()
	ss.init(q)
	var max_loaded := 0
	for i in range(6000):
		var x := float(i) * 3.0   # 3 u/frame ~ slow enough that unload window stays small
		ss.update(Vector3(x, 0, 0), 1.0 / 60.0)
		var lc: int = ss.stats()["loaded_chunks"]
		if lc > max_loaded:
			max_loaded = lc
	_check(max_loaded <= 45, "Integration: resident chunks bounded during walk-away (max %d)" % max_loaded)
	_check(ss.stats()["unload_count"] > 0, "Integration: chunks unloaded during walk-away (got %d)" % ss.stats()["unload_count"])
	_check(ss.stats()["load_count"] > 0, "Integration: chunks loaded during walk-away (got %d)" % ss.stats()["load_count"])
	ss.free()


func _report() -> void:
	if _check_count != EXPECTED_CHECKS:
		_failures.append(
			"assertion count %d != expected %d (a test likely threw before asserting)"
			% [_check_count, EXPECTED_CHECKS]
		)
	var lines: Array = ["resource_tests:", "checks=%d" % _check_count]
	var failed: bool = not _failures.is_empty()
	if failed:
		lines.append("RESULT=FAILED count=%d" % _failures.size())
		for f in _failures:
			lines.append("  - " + str(f))
	else:
		lines.append("RESULT=PASSED")
	var body: String = "\n".join(PackedStringArray(lines)) + "\n"
	var f := FileAccess.open(RESULT_PATH, FileAccess.WRITE)
	if f != null:
		f.store_string(body)
		f.close()
	print(body)
	quit(1 if failed else 0)
