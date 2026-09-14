## TASK-034 · Client Core — headless unit tests.
## Run: godot --headless --path client --script res://runtime/tests/test_client_core.gd
##
## Scope: pure-logic units that do NOT require the ProtocolCodec GDExtension.
## Snapshot-dependent assertions (apply_snapshot round-trip) are exercised by the
## integration test once client/extensions/protocol_codec is built (see its README).
##
## Two hard-won constraints baked into this file:
##  1. Types are annotated explicitly (never `:=`) for values taken off instances
##     created via `preload(...).new()` - GDScript cannot infer their member types.
##  2. EXPECTED_CHECKS is asserted. A test function that throws (e.g. a depended
##     script fails to compile) would otherwise leave _failures empty and report a
##     FALSE PASS. The count guard turns that into a hard failure.
extends SceneTree

## Results mirror file (Godot on Windows is a GUI-subsystem binary: stdout is not
## always capturable by the parent shell / verify script).
const RESULT_PATH := "res://runtime/tests/last_result.txt"

## Total number of _check() calls below; bump when adding assertions.
const EXPECTED_CHECKS := 14

var _failures: Array = []
var _check_count := 0


func _init() -> void:
	_test_game_loop_frame_accuracy()
	_test_game_loop_catchup_clamp()
	_test_config_defaults()
	_test_client_world_interpolation()
	_test_input_manager_null_camera()
	_test_net_client_framing()
	_test_protocol_codec_singleton()
	_report()


func _check(cond: bool, name: String) -> void:
	_check_count += 1
	if cond:
		print("PASS: ", name)
	else:
		print("FAIL: ", name)
		_failures.append(name)


func _test_game_loop_frame_accuracy() -> void:
	var gl = preload("res://runtime/autoload/game_loop.gd").new()
	var step: float = 1.0 / 60.0
	for _i in range(600):
		gl._physics_process(step)
	var frames: int = gl._frame_number
	gl.free()
	_check(frames == 600, "GameLoop: 600 steps -> 600 logic frames (got %d)" % frames)


func _test_game_loop_catchup_clamp() -> void:
	var gl = preload("res://runtime/autoload/game_loop.gd").new()
	# A huge stall must never produce more than MAX_CATCHUP steps (death spiral guard).
	gl._physics_process(10.0)
	var frames: int = gl._frame_number
	gl.free()
	_check(frames <= 5, "GameLoop: CatchUp clamped to <=5 steps on 10s stall (got %d)" % frames)


func _test_config_defaults() -> void:
	var cfg = preload("res://runtime/autoload/config.gd").new()
	cfg.config_path = "res://__nonexistent_config_dir__"
	var err: int = cfg.load_all()
	_check(err == ERR_FILE_NOT_FOUND, "Config: missing dir yields ERR_FILE_NOT_FOUND")
	_check(cfg.get_quality() == "low", "Config: default quality is low")
	_check(cfg.get_frame_cap() == 60, "Config: default frame cap is 60")
	cfg.free()


func _test_client_world_interpolation() -> void:
	var cw = preload("res://runtime/autoload/client_world.gd").new()
	# White-box seed: interpolation math only (decode path needs the GDExtension).
	cw._entities[1] = {"render_pos": Vector3.ZERO, "target_pos": Vector3(10, 0, 0)}
	cw.interpolate(0.02)  # t = clampf(dt * 10, 0, 1) = 0.2
	var p: Vector3 = cw.get_render_pos(1)
	_check(p.x > 0.0 and p.x < 10.0, "ClientWorld: partial interpolation (x=%.3f)" % p.x)
	for _i in range(80):
		cw.interpolate(0.05)
	var converged: Vector3 = cw.get_render_pos(1)
	cw.free()
	_check(
		converged.distance_to(Vector3(10, 0, 0)) < 0.001,
		"ClientWorld: converges onto server target (x=%.4f)" % converged.x
	)


func _test_input_manager_null_camera() -> void:
	var im = preload("res://runtime/autoload/input_manager.gd").new()
	var r: Dictionary = im.screen_to_world(null, Vector2.ZERO)
	im.free()
	_check(r.is_empty(), "InputManager: null camera returns empty ray dict")


func _test_net_client_framing() -> void:
	var nc = preload("res://network/net_client.gd").new()
	var b: PackedByteArray = nc._u32_bytes(258)
	var ok_size: bool = b.size() == 4
	var round_trip: int = b.decode_u32(0) if ok_size else 0
	_check(ok_size, "NetClient: length prefix is 4 bytes")
	_check(round_trip == 258, "NetClient: little-endian u32 round-trip (got %d)" % round_trip)
	_check(nc.state == nc.State.DISCONNECTED, "NetClient: initial state is DISCONNECTED")
	nc.free()


## End-to-end proof that the GDExtension binding is built and loaded: the
## ProtocolCodec singleton must be registered and able to encode/decode an envelope
## through the real TASK-005 FlatbuffCodec (no GDScript protocol re-implementation).
func _test_protocol_codec_singleton() -> void:
	var codec = Engine.get_singleton("ProtocolCodec")
	_check(codec != null, "ProtocolCodec GDExtension singleton registered")
	if codec == null:
		return
	var payload := PackedByteArray([1, 2, 3])
	var encoded: PackedByteArray = codec.encode_envelope(1, payload)
	_check(encoded.size() > 0, "ProtocolCodec: encode_envelope returns bytes (got %d)" % encoded.size())
	var env: Dictionary = codec.decode_envelope(encoded)
	_check(
		int(env.get("message_type", -1)) == 1,
		"ProtocolCodec: round-trip message_type=1 (got %d)" % int(env.get("message_type", -1))
	)


func _report() -> void:
	# Guard against false green: a test that throws before reaching _check() would
	# otherwise leave _failures empty while asserting nothing.
	if _check_count != EXPECTED_CHECKS:
		_failures.append(
			"assertion count %d != expected %d (a test likely threw before asserting)"
			% [_check_count, EXPECTED_CHECKS]
		)

	var lines: Array = ["client_core_tests:", "checks=%d" % _check_count]
	var failed: bool = not _failures.is_empty()
	if failed:
		lines.append("RESULT=FAILED count=%d" % _failures.size())
		for f in _failures:
			lines.append("  - " + str(f))
	else:
		lines.append("RESULT=PASSED")
	var body: String = "\n".join(PackedStringArray(lines)) + "\n"
	var f = FileAccess.open(RESULT_PATH, FileAccess.WRITE)
	if f != null:
		f.store_string(body)
		f.close()
	print(body)
	quit(1 if failed else 0)
