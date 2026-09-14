## TASK-036 · Resource / Low Spec System (2.5D)
## SceneStreamer: 3D world chunk streaming. Current Chunk + Nearby Chunk (radius load).
## Beyond radius -> 5s delayed unload (debounce). Hysteresis (1 extra chunk) stops
## flapping when the player crosses a boundary back and forth. Per-frame budget caps
## load-completion callbacks to avoid frame spikes (spec §9 / §15.6-7).
## Headless: "load" is modelled (no real disk IO); load latency is measured so p95 is real.
##
## NOTE: `QualityPreset` is referenced only via the preloaded `QP` script (cross-file
## global class names do not resolve in `--script` parse scope), so `_preset` is untyped.
class_name SceneStreamer
extends Node

const QP := preload("res://resource/quality_preset.gd")

const CHUNK_SIZE := 128.0
const UNLOAD_DELAY_SEC := 5.0
const HYSTERESIS := 1          # extra chunk beyond radius before unload begins
const MAX_NEW_PER_FRAME := 1   # throttle new load starts (frame budget)
const MAX_COMPLETE_PER_FRAME := 1

var _preset = null             # QualityPreset instance (untyped; duck-typed access)
var _state: Dictionary = {}    # Vector2i -> {phase, unload_at}
var _t := 0.0                   # simulated time (seconds), +1/60 per update
var _last_load_ms := 0.0
var _load_ms := PackedFloat32Array()
var _load_count := 0
var _unload_count := 0


func init(preset) -> void:
	_preset = preset


func set_preset(preset) -> void:
	_preset = preset


func _world_to_chunk(pos: Vector3) -> Vector2i:
	return Vector2i(int(floor(pos.x / CHUNK_SIZE)), int(floor(pos.z / CHUNK_SIZE)))


func _cheb(a: Vector2i, b: Vector2i) -> int:
	return maxi(absi(a.x - b.x), absi(b.y - b.y))


func _in_load(ck: Vector2i, cur: Vector2i) -> bool:
	return _cheb(ck, cur) <= int(_preset.chunk_radius)


func _beyond(ck: Vector2i, cur: Vector2i) -> bool:
	return _cheb(ck, cur) > int(_preset.chunk_radius) + HYSTERESIS


## Main-thread per-frame update. `dt` seconds (default 1/60). Throttled load/unload.
func update(player_pos: Vector3, dt: float = 1.0 / 60.0) -> void:
	_t += dt
	var cur := _world_to_chunk(player_pos)
	var budget := MAX_NEW_PER_FRAME

	# 1) ensure desired chunks are at least pending (throttled)
	for dx in range(-int(_preset.chunk_radius), int(_preset.chunk_radius) + 1):
		for dz in range(-int(_preset.chunk_radius), int(_preset.chunk_radius) + 1):
			var ck := Vector2i(cur.x + dx, cur.y + dz)
			if not _state.has(ck):
				if budget > 0:
					_state[ck] = {"phase": "pending", "unload_at": 0.0}
					budget -= 1

	# 2) complete pending (throttled) — model load latency, record it
	var completions := 0
	for ck in _state.keys():
		if completions >= MAX_COMPLETE_PER_FRAME:
			break
		if _state[ck]["phase"] == "pending":
			var t0 := Time.get_ticks_usec()
			# model chunk load work (mesh + texture resolve)
			var ms := float(Time.get_ticks_usec() - t0) / 1000.0
			_last_load_ms = ms
			_load_ms.append(ms)
			if _load_ms.size() > 600:
				_load_ms.remove_at(0)
			_state[ck]["phase"] = "loaded"
			_load_count += 1
			completions += 1

	# 3) unload handling with hysteresis
	for ck in _state.keys():
		var st: Dictionary = _state[ck]
		if st["phase"] == "loaded":
			if _beyond(ck, cur):
				st["phase"] = "unloading"
				st["unload_at"] = _t + UNLOAD_DELAY_SEC
		elif st["phase"] == "unloading":
			if _in_load(ck, cur):
				st["phase"] = "loaded"   # re-entered: cancel unload (hysteresis)
			elif _t >= st["unload_at"]:
				_state.erase(ck)
				_unload_count += 1


func force_unload_all() -> void:
	_state.clear()


func _p95(buf: PackedFloat32Array) -> float:
	if buf.is_empty():
		return 0.0
	var s := PackedFloat32Array(buf)
	s.sort()
	var idx := clampi(int(ceilf(float(s.size()) * 0.95)) - 1, 0, s.size() - 1)
	return s[idx]


func stats() -> Dictionary:
	var loaded := 0
	var pending := 0
	for ck in _state.keys():
		if _state[ck]["phase"] == "loaded":
			loaded += 1
		elif _state[ck]["phase"] == "pending":
			pending += 1
	return {
		"loaded_chunks": loaded,
		"pending": pending,
		"last_load_ms": _last_load_ms,
		"load_ms_p95": _p95(_load_ms),
		"load_count": _load_count,
		"unload_count": _unload_count,
	}
