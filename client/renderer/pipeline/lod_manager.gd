## TASK-035 · Renderer (2.5D)
## LODManager: distance-based 3-level LOD selection, threshold per quality preset.
## Shares view distance with TASK-036 SceneStreamer via QualityPreset (spec §27.2).
class_name LODManager
extends Node

const LEVELS := 3
var _view_dist := [80.0, 150.0, 250.0]
var _lod1 := [0.5, 0.6, 0.7]
var _lod2 := [0.8, 0.85, 0.9]
var _quality := 0


func set_quality(level: int) -> void:
	_quality = clampi(level, 0, LEVELS - 1)


func view_distance() -> float:
	return _view_dist[_quality]


## 0 = nearest (full), 1 = mid, 2 = far (downgraded/disabled per quality).
func decide(dist: float) -> int:
	var vd: float = _view_dist[_quality]
	if dist >= vd * _lod2[_quality]:
		return 2
	if dist >= vd * _lod1[_quality]:
		return 1
	return 0


func level_count() -> int:
	return LEVELS
