## TASK-035 · Renderer (2.5D)
## Culling: distance cull (beyond view distance => hide far sprites) + visible-index set.
## Keeps draw calls bounded by the Low budget (spec §8 / §22).
class_name Culling
extends Node

var _view_dist := 80.0


func set_view_distance(d: float) -> void:
	_view_dist = d


func is_visible(dist: float) -> bool:
	return dist <= _view_dist


## Returns indices (into positions) that fall within the view distance of cam_pos.
func cull(positions: Array, cam_pos: Vector3) -> Array:
	var visible: Array = []
	for i in range(positions.size()):
		var p: Vector3 = positions[i]
		if p.distance_to(cam_pos) <= _view_dist:
			visible.append(i)
	return visible
