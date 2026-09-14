## TASK-035 · Renderer (2.5D)
## SpriteEntity: 8-direction 2D sprite billboard (Sprite3D, no 3D model).
## Consumes the ClientWorld mirror read-only (spec §4 / §5). Presentation only:
## it never writes back to logic state.
class_name SpriteEntity
extends Node3D

enum Facing { DOWN, DOWN_RIGHT, RIGHT, UP_RIGHT, UP, UP_LEFT, LEFT, DOWN_LEFT }

var _sprite: Sprite3D = null
var _facing := 0
var _type := 0


func _init() -> void:
	# Create the billboard eagerly so it exists even before the node enters the tree
	# (unit tests assert sprite_node() right after .new()).
	_sprite = Sprite3D.new()
	# Sprite3D.BillboardMode: 2 = BILLBOARD_FIXED_Y (iso-facing, keeps yaw).
	_sprite.billboard = 2
	_sprite.centered = true


func _ready() -> void:
	add_child(_sprite)


## 8-way facing: DOWN/.. clockwise. Sets yaw so the billboard orients correctly.
func set_facing_8dir(dir: int) -> void:
	_facing = posmod(dir, 8)
	rotation.y = deg_to_rad(float(_facing) * 45.0)


func facing() -> int:
	return _facing


func set_type(t: int) -> void:
	_type = t


func set_texture(tex: Texture2D) -> void:
	if _sprite != null:
		_sprite.texture = tex


## Apply a mirror entity snapshot (read-only). ent keys: x,y,z,facing.
## Uses local position: the entity is a child of a container that owns world transform.
func apply_snapshot(ent: Dictionary) -> void:
	var x: float = float(ent.get("x", 0.0))
	var y: float = float(ent.get("y", 0.0))
	var z: float = float(ent.get("z", 0.0))
	position = Vector3(x, y, z)
	if ent.has("facing"):
		set_facing_8dir(int(ent["facing"]))


func sprite_node() -> Sprite3D:
	return _sprite
