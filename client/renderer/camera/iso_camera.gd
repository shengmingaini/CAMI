## TASK-035 · Renderer (2.5D)
## IsoCamera: Camera3D perspective with a fixed 45-degree pitch (user decision).
## Damped follow (never a hard per-frame snap) + ground ray pick for input/selection.
class_name IsoCamera
extends Camera3D

const PITCH_DEG := 45.0
const DEFAULT_DISTANCE := 24.0

var _target: Node3D = null
var _damping := 6.0


func _ready() -> void:
	# Perspective is the Camera3D default projection; fixed iso pitch via rotation below.
	keep_aspect = KEEP_HEIGHT
	fov = 50.0
	# Fixed iso pitch: look down at 45 degrees from horizontal.
	rotation.x = deg_to_rad(-PITCH_DEG)


## The user-decided fixed pitch angle (spec §2.2 / §13-1).
func pitch_deg() -> float:
	return PITCH_DEG


## Damped follow: ease toward the target each frame, never a hard cut.
## Uses local position (camera and target share the scene root frame).
func follow_target(node: Node3D, damping: float) -> void:
	_target = node
	_damping = maxf(damping, 0.0001)
	if _target != null:
		var back := Vector3(0.0, 0.0, DEFAULT_DISTANCE)
		var desired := _target.position + back
		var k := clampf(1.0 / _damping, 0.0, 1.0)
		position = position.lerp(desired, k)


## Ground pick: project the screen center ray onto the y=0 plane.
## Returns Vector3.ZERO if no viewport is available (headless unit-test safe).
func screen_to_world(ray_length: float) -> Vector3:
	var vp := get_viewport()
	if vp == null:
		return Vector3.ZERO
	var center: Vector2 = vp.size * 0.5
	var origin: Vector3 = project_ray_origin(center)
	var normal: Vector3 = project_ray_normal(center)
	if abs(normal.y) < 1e-6:
		return origin
	var t := -origin.y / normal.y
	var hit := origin + normal * t
	if ray_length > 0.0 and hit.distance_to(origin) > ray_length:
		return origin + normal * ray_length
	return hit
