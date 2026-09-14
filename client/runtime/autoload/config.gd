## TASK-034 · Client Core
## Config: loads config/client/*.json (resolution, frame cap, network, quality placeholder).
## Quality preset values are owned by TASK-036 (QualityPreset); this only loads the placeholder.
class_name Config
extends Node

const DEFAULT_QUALITY := "low"

var data: Dictionary = {}

## Directory holding client JSON config. Godot cannot read above res:// in exported
## builds; override via ProjectSettings or in tests. Defaults to repo config/client.
var config_path: String = "res://../config/client"


func _ready() -> void:
	load_all()


func load_all() -> int:
	_apply_defaults()
	var dir := DirAccess.open(config_path)
	if dir == null:
		push_warning("Config: directory not found: %s (using defaults)" % config_path)
		return ERR_FILE_NOT_FOUND
	dir.list_dir_begin()
	var fname := dir.get_next()
	while fname != "":
		if fname.ends_with(".json") and not fname.begins_with("."):
			var full := config_path.path_join(fname)
			var txt := FileAccess.get_file_as_string(full)
			if txt.length() > 0:
				var parsed = JSON.parse_string(txt)
				if typeof(parsed) == TYPE_DICTIONARY:
					data.merge(parsed, true)
		fname = dir.get_next()
	dir.list_dir_end()
	return OK


func _apply_defaults() -> void:
	if not data.has("resolution"):
		data["resolution"] = {"width": 1280, "height": 720}
	if not data.has("frame_cap"):
		data["frame_cap"] = 60
	if not data.has("quality"):
		data["quality"] = DEFAULT_QUALITY
	if not data.has("network"):
		data["network"] = {"host": "127.0.0.1", "port": 7000, "timeout_ms": 5000}


func get_quality() -> String:
	return str(data.get("quality", DEFAULT_QUALITY))


func get_network() -> Dictionary:
	return data.get("network", {}) as Dictionary


func get_frame_cap() -> int:
	return int(data.get("frame_cap", 60))
