## TASK-034 · Client Core
## GameLoop: fixed 60Hz logic frame driven by Godot `_physics_process`.
## Replaces the old self-built C++ main loop (TASK-034 2.5D refresh, docs/client-spec-2.5d.md §8).
## Logic frame 60Hz, CatchUp limited to MAX_CATCHUP steps to avoid death spiral.
class_name GameLoop
extends Node

## Fixed logic frequency (Hz).
const LOGIC_HZ := 60
## Maximum number of logic steps allowed to run in one render frame.
const MAX_CATCHUP := 5

## Emitted once per logic frame, before any system consumes the frame.
signal frame_start(ctx: Dictionary)

var _accumulator := 0.0
var _frame_number := 0
var _last_usec := 0
var _frame_ms_sum := 0.0
var _frame_samples := 0
var _longest_frame_ms := 0.0


func _ready() -> void:
	_last_usec = Time.get_ticks_usec()


func _physics_process(delta: float) -> void:
	var step := 1.0 / float(LOGIC_HZ)
	_accumulator += delta
	# CatchUp clamp: never allow more than MAX_CATCHUP pending steps.
	var max_accum := step * float(MAX_CATCHUP)
	if _accumulator > max_accum:
		_accumulator = max_accum
	while _accumulator >= step:
		_accumulator -= step
		_frame_number += 1
		var now := Time.get_ticks_usec()
		var frame_ms := float(now - _last_usec) / 1000.0
		_last_usec = now
		if frame_ms > _longest_frame_ms:
			_longest_frame_ms = frame_ms
		_frame_ms_sum += frame_ms
		_frame_samples += 1
		frame_start.emit({
			"dt_seconds": step,
			"frame_number": _frame_number,
			"now_ms": now,
		})


## Machine-readable frame statistics (consumed by TASK-034 benchmark / verify script).
func stats() -> Dictionary:
	var mean := 0.0
	if _frame_samples > 0:
		mean = _frame_ms_sum / float(_frame_samples)
	return {
		"fps": Engine.get_frames_per_second(),
		"logic_fps": LOGIC_HZ,
		"frame_ms": mean,
		"longest_frame_ms": _longest_frame_ms,
		"frame_number": _frame_number,
	}


func reset_stats() -> void:
	_frame_ms_sum = 0.0
	_frame_samples = 0
	_longest_frame_ms = 0.0
