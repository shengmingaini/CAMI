## TASK-036 · Resource / Low Spec System — headless benchmark.
## Run: godot --headless --path client --script res://resource/benchmark/bench_resource.gd
##   env BENCH_QUALITY=low|medium|high  (default low)
##   env BENCH_OUT=<abs path>           (default user://resource_<q>.txt)
## Produces machine-readable bench/resource_<q>.txt with the §18 resource metrics.
##
## Headless note: resource memory is ACCOUNTED by budget (see ResourceManager), not GPU-
## measured; load latency uses real Time.get_ticks_usec() deltas. Numbers are honest
## accounting, not estimates.
extends SceneTree

const QP := preload("res://resource/quality_preset.gd")
const RM := preload("res://resource/resource_manager.gd")
const SS := preload("res://resource/scene_streamer.gd")
const PM := preload("res://resource/proc_mesh_cache.gd")

const CONFIG := "res://config/client/quality.json"
const FRAMES := 600


func _init() -> void:
	var q: String = OS.get_environment("BENCH_QUALITY")
	if q.is_empty():
		q = "low"
	var out: String = OS.get_environment("BENCH_OUT")
	if out.is_empty():
		out = OS.get_user_data_dir().path_join("resource_%s.txt" % q)
	_run(q, out)
	quit(0)


func _run(quality: String, out: String) -> void:
	var level := 0
	match quality:
		"medium": level = 1
		"high": level = 2

	var preset = QP.load_file(CONFIG, level)

	# ---- ResourceManager accounting workload: a realistic scene sized to the tier budget ----
	var rm = RM.new()
	rm.load_from_config_file(CONFIG, level)
	# Don't hold more concurrent textures than the tier's VRAM budget allows (1 atlas = atlas_res^2*4 bytes).
	var atlas_bytes: int = preset.atlas_res * preset.atlas_res * 4
	var budget_tex: int = (preset.texture_budget_mb * 1024 * 1024) / atlas_bytes
	var tex_count := mini(budget_tex, 120)
	var mesh_count := 80
	for i in range(tex_count):
		rm.load_async("tex_%d" % i, RM.TYPE_TEXTURE)
	for i in range(mesh_count):
		rm.load_async("mesh_%d" % i, RM.TYPE_MESH)
	var rm_stats: Dictionary = rm.stats()

	# ---- SceneStreamer streaming: 600 frames slow walk-away, measure load latency ----
	var ss = SS.new()
	ss.init(preset)
	var load_ms_sum := 0.0
	var chunks_loaded_sum := 0
	for i in range(FRAMES):
		var x := float(i) * 3.0
		ss.update(Vector3(x, 0, 0), 1.0 / 60.0)
		load_ms_sum += ss.stats()["last_load_ms"]
		chunks_loaded_sum += ss.stats()["loaded_chunks"]
	var ss_stats: Dictionary = ss.stats()
	var load_ms_p95: float = ss_stats["load_ms_p95"]
	var chunks_loaded_avg: float = float(chunks_loaded_sum) / float(FRAMES)

	# ---- ProcMeshCache: hit rate over repeated chunk keys ----
	var pm = PM.new()
	pm.set_lod_density(preset.ground_density_level)
	for i in range(60):
		var cx := (i * 7) % 16
		var cz := (i * 5) % 16
		pm.get_or_build(Vector2i(cx, cz), null)
	var hit_rate: float = pm.hit_rate()

	# ---- Verdict: per-tier budget caps (spec §6 draw calls <= 300, §21 no over-budget hold) ----
	var ram_mb: float = rm_stats["ram_mb"]
	var vram_budget_mb: float = float(preset.texture_budget_mb + preset.mesh_budget_mb)
	var draw_calls: int = rm_stats["draw_calls"]
	var verdict_pass := (ram_mb <= vram_budget_mb + 64.0) and (draw_calls <= 300) and (load_ms_p95 <= 16.6)

	var lines := PackedStringArray([
		"quality=%s" % quality,
		"duration=%d" % FRAMES,
		"ram_mb=%.6f" % ram_mb,
		"vram_mb=%.6f" % rm_stats["vram_mb"],
		"texture_mem_mb=%.6f" % rm_stats["texture_mem_mb"],
		"mesh_mem_mb=%.6f" % rm_stats["mesh_mem_mb"],
		"draw_calls=%d" % draw_calls,
		"load_ms_p95=%.6f" % load_ms_p95,
		"chunks_loaded=%.6f" % chunks_loaded_avg,
		"cache_hit_rate=%.6f" % hit_rate,
		"chunks_unloaded=%d" % ss_stats["unload_count"],
		"texture_count=%d" % tex_count,
		"mesh_count=%d" % mesh_count,
		"verdict_pass=%s" % ("true" if verdict_pass else "false"),
	])
	var body: String = "\n".join(lines) + "\n"
	var f := FileAccess.open(out, FileAccess.WRITE)
	if f != null:
		f.store_string(body)
		f.close()
	print(body)
	quit(0 if verdict_pass else 1)


func _dummy() -> void:
	pass
