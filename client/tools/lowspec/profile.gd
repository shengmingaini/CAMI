## TASK-036 · Low-Spec Profiler (headless capability probe).
## Run: godot --headless --path client --script res://tools/lowspec/profile.gd
##   env PROFILE_OUT=<abs path>   (default user://lowspec_profile.txt)
## Detects available system RAM, picks the highest preset that fits the §35 Low target
## (4 GB RAM / 1 GB VRAM class), loads it, and writes a machine-readable profile report.
extends SceneTree

const QP := preload("res://resource/quality_preset.gd")
const CONFIG := "res://config/client/quality.json"


func _init() -> void:
	var out: String = OS.get_environment("PROFILE_OUT")
	if out.is_empty():
		out = OS.get_user_data_dir().path_join("lowspec_profile.txt")
	_run(out)
	quit(0)


func _run(out: String) -> void:
	# Detect physical RAM (Godot 4.3+). Returns {physical, free}; 0 if unavailable headless.
	var phys_ram_mb := 0
	if OS.has_method("get_memory_info"):
		var info: Dictionary = OS.get_memory_info()
		phys_ram_mb = int(info.get("physical", 0)) / (1024 * 1024)

	# Pick the highest preset whose texture+mesh budget fits comfortably in detected RAM.
	# Low target (§35): 4 GB RAM, 1 GB VRAM class.
	var chosen := 0   # default low
	if phys_ram_mb >= 8192:
		chosen = 2
	elif phys_ram_mb >= 4096:
		chosen = 1
	# If detection failed (0), stay conservative on low.

	var preset = QP.load_file(CONFIG, chosen)
	var vram_budget_mb: int = preset.texture_budget_mb + preset.mesh_budget_mb

	var fits: bool = (phys_ram_mb == 0) or (phys_ram_mb >= (vram_budget_mb + 512))

	var lines := PackedStringArray([
		"detected_ram_mb=%d" % phys_ram_mb,
		"chosen_preset=%s" % preset.name(),
		"chosen_level=%d" % preset.level,
		"vram_budget_mb=%d" % vram_budget_mb,
		"texture_cap=%d" % preset.texture_cap,
		"atlas_res=%d" % preset.atlas_res,
		"chunk_radius=%d" % preset.chunk_radius,
		"triangle_budget=%d" % preset.triangle_budget,
		"shadow=%s" % ("true" if preset.shadow else "false"),
		"fits_low_target=%s" % ("true" if fits else "false"),
		"verdict_pass=%s" % ("true" if (preset.level <= 2 and not preset.shadow) else "false"),
	])
	var body: String = "\n".join(lines) + "\n"
	var f := FileAccess.open(out, FileAccess.WRITE)
	if f != null:
		f.store_string(body)
		f.close()
	print(body)
