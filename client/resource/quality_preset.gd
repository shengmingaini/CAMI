## TASK-036 · Resource / Low Spec System (2.5D)
## QualityPreset: three config-driven presets (Low/Medium/High). No hard-coded params
## (spec §21 Forbidden: "禁止硬编码画质参数"). Loaded from config/client/quality.json.
##
## NOTE: no `class_name` — global class names do NOT resolve in `--script` parse scope, so
## all callers use `preload("res://resource/quality_preset.gd")` (see QP alias). `load_file`
## self-instantiates via the same preload to avoid referencing the class by name.
extends Resource

# ---- §8 three-preset fields ----
var level := 0                 # 0=low, 1=medium, 2=high
var texture_cap := 512         # atlas/texture max resolution
var ground_density_level := 0  # 0=large cells, 1=mid, 2=small/detailed
var lod_aggressive := true     # far-sprite downgrade aggressiveness
var view_dist := 80.0          # meters
var chunk_radius := 1          # 3D world chunk streaming radius (Current + Nearby)
var texture_budget_mb := 256   # VRAM budget for textures/atlases
var mesh_budget_mb := 128      # VRAM budget for meshes
var triangle_budget := 300000  # 2.5D triangle budget (Low < 300k)
var atlas_res := 512           # sprite atlas resolution
var same_screen_entities := 50 # on-screen entity cap
var particles := 200           # particle cap
var net_rate := 10             # network update rate (Hz)
var shadow := false            # shadows (off on all tiers, ambient approx)

const NAMES := ["low", "medium", "high"]


## Build a preset from a parsed quality.json Dictionary at the given level.
func load_config(cfg: Dictionary, lvl: int) -> void:
	level = clampi(lvl, 0, 2)
	var key: String = NAMES[level]
	var p: Dictionary = cfg.get("presets", {}).get(key, {})
	texture_cap = int(p.get("texture_cap", 512))
	ground_density_level = int(p.get("ground_density_level", 0))
	lod_aggressive = bool(p.get("lod_aggressive", true))
	view_dist = float(p.get("view_dist", 80.0))
	chunk_radius = int(p.get("chunk_radius", 1))
	texture_budget_mb = int(p.get("texture_budget_mb", 256))
	mesh_budget_mb = int(p.get("mesh_budget_mb", 128))
	triangle_budget = int(p.get("triangle_budget", 300000))
	atlas_res = int(p.get("atlas_res", 512))
	same_screen_entities = int(p.get("same_screen_entities", 50))
	particles = int(p.get("particles", 200))
	net_rate = int(p.get("net_rate", 10))
	shadow = bool(p.get("shadow", false))


## Static helper: read config/client/quality.json and build the preset for `lvl`.
static func load_file(path: String, lvl: int) -> Resource:
	var q: Resource = load("res://resource/quality_preset.gd").new()
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		# Config missing -> fall back to embedded defaults so the pipeline still runs.
		q.load_config({"presets": {}}, lvl)
		return q
	var text := f.get_as_text()
	f.close()
	var parsed: Variant = JSON.parse_string(text)
	if parsed == null or typeof(parsed) != TYPE_DICTIONARY:
		q.load_config({"presets": {}}, lvl)
		return q
	q.load_config(parsed, lvl)
	return q


## Grid resolution per ground-density level (cells per chunk side). Smaller cell = denser.
func ground_grid() -> int:
	match ground_density_level:
		0: return 16
		1: return 24
		_: return 32


func name() -> String:
	return NAMES[level]
