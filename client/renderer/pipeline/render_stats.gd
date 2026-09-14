## TASK-035 · Renderer (2.5D)
## RenderStats: per-frame render counters + p95 frame-time sampling.
## Counters are fed by the pipeline (real scene-graph counts); frame cpu/gpu time is
## measured by the benchmark loop. sample() aggregates them into the §18 metric dict.
class_name RenderStats
extends Node

var _quality := 0
var _draw_calls := 0
var _triangles := 0
var _texture_mem_mb := 0.0
var _mesh_mem_mb := 0.0
var _shader_switches := 0
var _visible_sprites := 0
var _cpu := PackedFloat32Array()
var _gpu := PackedFloat32Array()
var _max_samples := 600


## Consume TASK-036 QualityPreset level (Low/Medium/High).
func set_quality(level: int) -> void:
	_quality = level


func set_counts(draw_calls: int, triangles: int, texture_mem_mb: float, mesh_mem_mb: float, shader_switches: int, visible_sprites: int) -> void:
	_draw_calls = draw_calls
	_triangles = triangles
	_texture_mem_mb = texture_mem_mb
	_mesh_mem_mb = mesh_mem_mb
	_shader_switches = shader_switches
	_visible_sprites = visible_sprites


func record_frame(cpu_ms: float, gpu_ms: float) -> void:
	_cpu.append(cpu_ms)
	_gpu.append(gpu_ms)
	if _cpu.size() > _max_samples:
		_cpu.remove_at(0)
		_gpu.remove_at(0)


func _p95(buf: PackedFloat32Array) -> float:
	if buf.is_empty():
		return 0.0
	var s := PackedFloat32Array(buf)
	s.sort()
	var idx := int(ceilf(float(s.size()) * 0.95)) - 1
	idx = clampi(idx, 0, s.size() - 1)
	return s[idx]


## §18 benchmark metric dict.
func sample() -> Dictionary:
	var cpu_p95 := _p95(_cpu)
	return {
		"draw_calls": _draw_calls,
		"triangles": _triangles,
		"texture_mem_mb": _texture_mem_mb,
		"mesh_mem_mb": _mesh_mem_mb,
		"shader_switches": _shader_switches,
		"visible_sprites": _visible_sprites,
		"cpu_ms": cpu_p95,
		"gpu_ms": _p95(_gpu),
		"fps_p95": 0.0 if cpu_p95 <= 0.0 else 1000.0 / cpu_p95,
		"quality": _quality,
	}
