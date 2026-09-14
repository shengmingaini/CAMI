## TASK-034 · Client Core — headless INTEGRATION test.
## Run: godot --headless --path client --script res://runtime/tests/test_client_integration.gd
##
## Exercises the REAL component chain that task book §17 / §19 care about:
##   NetClient._on_packet(frame)              (envelope resolution hook, post-socket)
##     -> ProtocolCodec GDExtension (TASK-005 FlatbuffCodec) decode
##     -> ClientWorld mirror + interpolation smoothing
##     -> corrupt snapshot discarded, no crash
##     -> reconnect state machine (freeze, not crash)
##
## NOTE on the loopback socket leg: the full connect->push-over-TCP->poll path
## needs a real loopback socket. On THIS sandbox the headless Godot socket connect
## never completes (stays STATUS_CONNECTING), so the raw socket half is verified
## only by the unit-test framing checks and must be run in CI/on a host with
## working loopback. Everything past the socket (decode + mirror + reconnect) is
## driven here directly through NetClient._on_packet, which is exactly the entry
## point the socket reader calls.
##
## REQUIRES the protocol_codec GDExtension built (scons aoi_snapshot=1).
extends SceneTree

const RESULT_PATH := "res://runtime/tests/last_integration_result.txt"
const ENTITY_COUNT := 5

var _failures: Array = []
var _check_count := 0
var _done := false

var _cw = null
var _nc = null
var _codec = null
var _phase := 0
var _snapshots := 0
var _corrupt_dropped := false


func _initialize() -> void:
	_codec = Engine.get_singleton("ProtocolCodec")
	if _codec == null:
		_check(false, "ProtocolCodec singleton missing - build protocol_codec with scons aoi_snapshot=1")
		_report()
		_done = true
		return
	_cw = preload("res://runtime/autoload/client_world.gd").new()
	_nc = preload("res://network/net_client.gd").new()
	_nc.message_received.connect(_on_message)

	# 1) Real snapshot frame through the decode path (as the socket reader would deliver).
	var frame: PackedByteArray = _codec.encode_test_aoi_batch(ENTITY_COUNT)
	_nc._on_packet(frame)

	# 2) Corrupt body (valid envelope would be required; garbage must be dropped).
	_nc._on_packet(PackedByteArray([0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03]))

	# 3) Reconnect state machine (what the socket half triggers on disconnect).
	_nc._enter_reconnect()


func _on_message(_opcode: int, frame: PackedByteArray) -> void:
	var rc: int = _cw.apply_snapshot(frame)
	if rc == OK:
		_snapshots += 1
	elif rc == ERR_INVALID_DATA:
		_corrupt_dropped = true


func _process(_delta: float) -> bool:
	if _done:
		return true
	match _phase:
		0:
			_check(_cw.entity_count() == ENTITY_COUNT,
				"snapshot: mirror holds %d entities (got %d)" % [ENTITY_COUNT, _cw.entity_count()])
			_phase = 1
		1:
			_check(_snapshots >= 1, "snapshot: %d frame(s) applied via real codec" % _snapshots)
			_phase = 2
		2:
			# Smoothing must LAG the server target (100ms interpolation buffer):
			# the mirror must not teleport straight onto the target on first frame.
			var target: Vector3 = Vector3(ENTITY_COUNT - 1.0, 0.0, ENTITY_COUNT - 1.0)
			var rendered: Vector3 = _cw.get_render_pos(ENTITY_COUNT)
			_check(rendered.distance_to(target) > 0.001,
				"interpolation: render lags target (rendered=%s target=%s)" % [rendered, target])
			_phase = 3
		3:
			_check(_corrupt_dropped, "corrupt snapshot dropped (no crash)")
			_phase = 4
		4:
			_check(_nc.state == _nc.State.RECONNECTING,
				"disconnect: client entered RECONNECTING (freeze, not crash)")
			_phase = 5
		5:
			_report()
			_done = true
	return _done


func _check(cond: bool, name: String) -> void:
	_check_count += 1
	if cond:
		print("PASS: ", name)
	else:
		print("FAIL: ", name)
		_failures.append(name)


func _report() -> void:
	var lines: Array = ["client_integration_tests:", "checks=%d" % _check_count, "snapshots=%d" % _snapshots]
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
