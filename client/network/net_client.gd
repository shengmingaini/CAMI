## TASK-034 · Client Core
## NetClient: client-side transport. Main-thread driven; no worker thread parses business
## packets (spec: "禁止网络线程解析业务包（只搬运）"). Uses TASK-005 protocol via the
## ProtocolCodec GDExtension singleton.
class_name NetClient
extends Node

enum State { DISCONNECTED, CONNECTING, CONNECTED, RECONNECTING }

const MAX_PACKET_BYTES := 16 * 1024 * 1024
const MAX_RECONNECT_DELAY := 5.0

signal connected
signal disconnected
signal message_received(opcode: int, payload: PackedByteArray)

var state := State.DISCONNECTED
var rtt_ms := 0

var _stream: StreamPeerTCP = null
var _host := "127.0.0.1"
var _port := 7000
var _reconnect_attempts := 0
var _reconnect_timer := 0.0


func _physics_process(delta: float) -> void:
	if _stream == null:
		return
	match state:
		State.CONNECTING:
			var st := _stream.get_status()
			if st == StreamPeerTCP.STATUS_CONNECTED:
				state = State.CONNECTED
				_reconnect_attempts = 0
				connected.emit()
			elif st == StreamPeerTCP.STATUS_ERROR:
				_enter_reconnect()
		State.CONNECTED:
			if _stream.get_status() != StreamPeerTCP.STATUS_CONNECTED:
				_enter_reconnect()
			else:
				_poll_read()
		State.RECONNECTING:
			_reconnect_timer -= delta
			if _reconnect_timer <= 0.0:
				connect_to(_host, _port)


func connect_to(host: String, port: int) -> void:
	_host = host
	_port = port
	if _stream != null:
		_stream.disconnect_from_host()
	_stream = StreamPeerTCP.new()
	state = State.CONNECTING
	var err := _stream.connect_to_host(host, port)
	if err != OK:
		state = State.DISCONNECTED
		push_error("NetClient: connect_to_host failed err=%d" % err)


## Main-thread poll. Reading is performed inline via StreamPeerTCP (non-blocking);
## this returns no queued events because `_poll_read` emits `message_received` directly.
func poll() -> Array:
	if state == State.CONNECTED and _stream != null:
		_poll_read()
	return []


func _poll_read() -> void:
	if _stream == null:
		return
	# Wire framing: 4-byte big-endian length prefix + envelope bytes.
	# NOTE: exact framing must match gateway/TASK-005; validated by the integration test.
	while _stream.get_available_bytes() >= 4:
		var head := _stream.get_partial_data(4)
		if head[0] != OK or (head[1] as PackedByteArray).size() < 4:
			return
		var pkt_len := (head[1] as PackedByteArray).decode_u32(0)
		if pkt_len <= 0 or pkt_len > MAX_PACKET_BYTES:
			push_error("NetClient: invalid frame length %d" % pkt_len)
			_enter_reconnect()
			return
		if _stream.get_available_bytes() < pkt_len:
			return
		var body := _stream.get_partial_data(pkt_len)
		if body[0] != OK:
			return
		_on_packet(body[1] as PackedByteArray)


func _on_packet(bytes: PackedByteArray) -> void:
	var opcode := 0
	# Resolved at runtime: ProtocolCodec is a GDExtension singleton, not a global class.
	var codec = Engine.get_singleton("ProtocolCodec")
	if codec != null:
		var env = codec.decode_envelope(bytes)
		if typeof(env) == TYPE_DICTIONARY:
			opcode = int(env.get("message_type", 0))
	# Emit the COMPLETE envelope frame (not the inner payload): ClientWorld.apply_snapshot
	# decodes the envelope itself (contract in TASK-034 §7). The opcode is still resolved
	# here so consumers can route without decoding twice.
	message_received.emit(opcode, bytes)


func send(opcode: int, payload: PackedByteArray) -> void:
	if state != State.CONNECTED or _stream == null:
		return
	var frame: PackedByteArray = payload
	var codec = Engine.get_singleton("ProtocolCodec")
	if codec != null:
		frame = codec.encode_envelope(opcode, payload) as PackedByteArray
	var out := _u32_bytes(frame.size())
	out.append_array(frame)
	_stream.put_data(out)


## 4-byte length prefix for the wire frame.
##
## Byte order: LITTLE-endian. PackedByteArray.decode_u32() (used by _poll_read)
## defaults to little-endian, and the C++ gateway writes a native uint32_t on x86,
## so the two sides agree. If the gateway ever switches to network byte order,
## BOTH this function and the decode call must change together.
func _u32_bytes(v: int) -> PackedByteArray:
	return PackedByteArray([
		v & 0xFF,
		(v >> 8) & 0xFF,
		(v >> 16) & 0xFF,
		(v >> 24) & 0xFF,
	])


func _enter_reconnect() -> void:
	if state == State.RECONNECTING:
		return
	state = State.RECONNECTING
	_reconnect_attempts += 1
	_reconnect_timer = minf(0.5 * float(_reconnect_attempts), MAX_RECONNECT_DELAY)
	disconnected.emit()
