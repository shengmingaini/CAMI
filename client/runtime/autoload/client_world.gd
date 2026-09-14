## TASK-034 · Client Core
## ClientWorld: local mirror of server state + interpolation.
## DISCIPLINE (RFC §9.6 / client-spec-2.5d.md §1): the client performs NO authoritative
## judgment (damage/hit/drop are server-side) and the logic layer must not resolve
## presentation. This only mirrors and smooths what the server sends.
class_name ClientWorld
extends Node

## Interpolation buffer delay (ms).
const INTERP_DELAY_MS := 100.0
## Extrapolation cap (ms) - beyond this the entity is frozen.
const EXTRAPOLATE_MAX_MS := 200.0
const MAX_SNAPSHOT_BUFFER := 64
## ~100ms smoothing rate used by interpolate().
const SMOOTH_RATE_HZ := 10.0

var local_player_id := -1

var _entities: Dictionary = {}
var _snapshots: Array = []


func _physics_process(delta: float) -> void:
	interpolate(delta)


## Apply a FlatBuffers snapshot. Decoding goes through the GDExtension ProtocolCodec
## singleton (reuses TASK-005 codec); never re-implemented in GDScript.
func apply_snapshot(fb: PackedByteArray) -> int:
	var snap := _decode(fb)
	if snap.is_empty():
		return ERR_INVALID_DATA
	snap["_recv_ms"] = Time.get_ticks_msec()
	_snapshots.append(snap)
	if _snapshots.size() > MAX_SNAPSHOT_BUFFER:
		_snapshots.pop_front()
	if snap.has("local_player_id"):
		local_player_id = int(snap["local_player_id"])
	var recv_ms := int(snap["_recv_ms"])
	# Entities that left the AOI are dropped from the mirror.
	for gone in snap.get("leaves", []):
		_entities.erase(int(gone))
	for e in snap.get("entities", []):
		var id := int(e.get("id", -1))
		if id < 0:
			continue
		if not _entities.has(id):
			_entities[id] = {"render_pos": Vector3.ZERO}
		var ent: Dictionary = _entities[id]
		ent["target_pos"] = Vector3(
			float(e.get("x", 0.0)),
			float(e.get("y", 0.0)),
			float(e.get("z", 0.0))
		)
		ent["last_update_ms"] = recv_ms
	return OK


func _decode(fb: PackedByteArray) -> Dictionary:
	# The GDExtension singleton only exists at runtime (and only once the
	# protocol_codec library is built), so it must be resolved via
	# Engine.get_singleton - a bare `ProtocolCodec` identifier is a parse error
	# because it is not a registered global class.
	var codec = Engine.get_singleton("ProtocolCodec")
	if codec != null:
		var snap = codec.decode_snapshot(fb)
		if typeof(snap) == TYPE_DICTIONARY:
			return snap
		return {}
	push_error(
		"ClientWorld: ProtocolCodec GDExtension singleton unavailable; "
		+ "cannot decode snapshot (build client/extensions/protocol_codec)"
	)
	return {}


## Smooth positions toward the last server target (100ms buffer semantics).
func interpolate(dt: float) -> void:
	var t: float = clampf(dt * SMOOTH_RATE_HZ, 0.0, 1.0)
	for id in _entities.keys():
		var ent: Dictionary = _entities[id]
		if ent.has("target_pos"):
			ent["render_pos"] = (ent["render_pos"] as Vector3).lerp(ent["target_pos"] as Vector3, t)


func get_render_pos(id: int) -> Vector3:
	if _entities.has(id):
		return _entities[id].get("render_pos", Vector3.ZERO) as Vector3
	return Vector3.ZERO


func entity_count() -> int:
	return _entities.size()


func local_player() -> int:
	return local_player_id
