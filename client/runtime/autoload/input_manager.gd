## TASK-034 · Client Core
## InputManager: samples input exactly once per logic frame (spec: "input only sampled at
## logic frame start"). Keyboard/mouse/gamepad abstraction; screen->world ray for picking.
class_name InputManager
extends Node

## Last sampled movement axis (logic-frame coherent).
var move_vector := Vector2.ZERO


func _ready() -> void:
	# NOTE: `class_name GameLoop` shadows the autoload identifier, so the global
	# name `GameLoop` resolves to the *class*, not the autoload node - connecting
	# via `GameLoop.frame_start` is a parse error. Resolve through the scene tree.
	var gl := get_node_or_null("/root/GameLoop")
	if gl != null and gl.has_signal("frame_start"):
		gl.frame_start.connect(_on_frame_start)
	else:
		push_warning("InputManager: /root/GameLoop not found; frame_start not connected")


func _on_frame_start(_ctx: Dictionary) -> void:
	_sample()


func _sample() -> void:
	move_vector = Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	# Mouse / gamepad abstractions are reserved for later client tasks.


## Screen (mouse) position -> world ray. Returns {origin, dir, end}.
## Caller resolves the actual intersection (e.g. ground plane or PhysicsDirectSpaceState3D).
func screen_to_world(camera: Camera3D, mouse_pos: Vector2, ray_length: float = 1000.0) -> Dictionary:
	if camera == null:
		return {}
	var origin := camera.project_ray_origin(mouse_pos)
	var dir := camera.project_ray_normal(mouse_pos)
	return {
		"origin": origin,
		"dir": dir,
		"end": origin + dir * ray_length,
	}
