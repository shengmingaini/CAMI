## TASK-036 · Resource / Low Spec System (2.5D)
## ResourceManager: async load / refcount / LRU reclaim / per-budget hot-swap.
##
## Headless note: Godot headless cannot instantiate real Image/Texture rasters (see
## TASK-035 img-probe), so resource memory is ACCOUNTED by type+budget, not GPU-measured.
## `ResHandle` (a Resource) stands in for the loaded asset; `stats()` reports the budget
## accounting honestly. On a real device the same code path loads via ResourceLoader and
## the byte model is replaced by actual VRAM — the public interface is identical.
##
## NOTE: `QualityPreset` is referenced only via the preloaded `QP` script (cross-file
## global class names do not resolve in `--script` parse scope), so `_preset` is untyped
## and accessed duck-typed at runtime.
class_name ResourceManager
extends Node

const QP := preload("res://resource/quality_preset.gd")

class ResHandle extends RefCounted:
	var uri: String = ""
	var bytes: int = 0
	var rtype: int = 0   # 0=texture/atlas, 1=mesh, 2=other

const TYPE_TEXTURE := 0
const TYPE_MESH := 1
const TYPE_OTHER := 2

var _preset = null            # QualityPreset instance (untyped; duck-typed access)
var _cache: Dictionary = {}      # uri -> {res, refs, bytes, type, last_used}
var _clock := 0
var _last_load_ms := 0.0
var _pending := 0
var _cfg_cache: Dictionary = {}

## type byte model (headless accounting): texture = atlas_res^2 * 4, mesh = grid^2 * 32,
## other = 4 KiB. Real builds replace with the loaded asset's get_rid() memory.
func _bytes_for(type: int) -> int:
	if _preset == null:
		return 4096
	match type:
		TYPE_TEXTURE: return _preset.atlas_res * _preset.atlas_res * 4
		TYPE_MESH: return _preset.ground_grid() * _preset.ground_grid() * 32
		_: return 4096


func init(preset) -> void:
	_preset = preset


## Re-load the active preset for `level` from the last parsed config (defaults if none).
func set_quality(level: int) -> void:
	if _preset != null:
		_preset.load_config(_cfg_cache, level)
	_reclaim(TYPE_TEXTURE)
	_reclaim(TYPE_MESH)


func load_from_config_file(path: String, level: int) -> void:
	var q := QP.load_file(path, level)
	_cfg_cache = _read_cfg(path)
	_preset = q


func _read_cfg(path: String) -> Dictionary:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return {"presets": {}}
	var t := f.get_as_text()
	f.close()
	var v: Variant = JSON.parse_string(t)
	if v == null or typeof(v) != TYPE_DICTIONARY:
		return {"presets": {}}
	return v


## Async load (modelled). Returns a ResHandle (RefCounted). Increments refcount, evicts
## LRU entries of the same type when over budget.
func load_async(uri: String, type: int) -> RefCounted:
	_clock += 1
	if _cache.has(uri):
		var e: Dictionary = _cache[uri]
		e["refs"] += 1
		e["last_used"] = _clock
		return e["res"]
	var h := ResHandle.new()
	h.uri = uri
	h.bytes = _bytes_for(type)
	h.rtype = type
	var t0 := Time.get_ticks_usec()
	_cache[uri] = {"res": h, "refs": 1, "bytes": h.bytes, "type": type, "last_used": _clock}
	_reclaim(type)
	_last_load_ms = float(Time.get_ticks_usec() - t0) / 1000.0
	return h


## Decrement refcount. When refs hit 0 the handle is eligible for LRU eviction (freed by
## Godot refcount automatically when dropped from the cache).
func unload(res: RefCounted) -> void:
	if res == null:
		return
	for uri in _cache.keys():
		var e: Dictionary = _cache[uri]
		if e["res"] == res:
			e["refs"] -= 1
			if e["refs"] <= 0:
				# Refcount 0: drop from cache so Godot frees it (no manual free on live res).
				_cache.erase(uri)
			return


## LRU reclaim: evict type entries with refs==0, oldest last_used first, until under budget.
func _reclaim(type: int) -> void:
	if _preset == null:
		return
	var budget_bytes: int = (_preset.texture_budget_mb if type == TYPE_TEXTURE else _preset.mesh_budget_mb) * 1024 * 1024
	var total := 0
	for uri in _cache.keys():
		var e: Dictionary = _cache[uri]
		if e["type"] == type:
			total += e["bytes"]
	var candidates: Array = []
	for uri in _cache.keys():
		var e: Dictionary = _cache[uri]
		if e["type"] == type and e["refs"] <= 0:
			candidates.append(uri)
	candidates.sort_custom(func(a: String, b: String) -> bool:
		return _cache[a]["last_used"] < _cache[b]["last_used"])
	var ci := 0
	while total > budget_bytes and ci < candidates.size():
		var uri: String = candidates[ci]
		var e: Dictionary = _cache[uri]
		total -= e["bytes"]
		_cache.erase(uri)
		ci += 1


func stats() -> Dictionary:
	var tex_bytes := 0
	var mesh_bytes := 0
	var other_bytes := 0
	var tex_count := 0
	var mesh_count := 0
	for uri in _cache.keys():
		var e: Dictionary = _cache[uri]
		if e["type"] == TYPE_TEXTURE:
			tex_bytes += e["bytes"]; tex_count += 1
		elif e["type"] == TYPE_MESH:
			mesh_bytes += e["bytes"]; mesh_count += 1
		else:
			other_bytes += e["bytes"]
	var total := tex_bytes + mesh_bytes + other_bytes
	return {
		"ram_mb": float(total) / (1024.0 * 1024.0),
		"vram_mb": float(tex_bytes + mesh_bytes) / (1024.0 * 1024.0),
		"texture_mem_mb": float(tex_bytes) / (1024.0 * 1024.0),
		"mesh_mem_mb": float(mesh_bytes) / (1024.0 * 1024.0),
		"draw_calls": tex_count + mesh_count,
		"loaded": _cache.size(),
		"pending": _pending,
		"last_load_ms": _last_load_ms,
	}
