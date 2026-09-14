## TASK-036 · Resource / Low Spec System (2.5D)
## ProcMeshCache: procedural ground mesh cache. No DCC models (spec §21). One ArrayMesh
## per world chunk, keyed by chunk_key; cache hit reuses the mesh instead of rebuilding.
## Headless: builds a flat grid (Image heightmap raster is unavailable headless); a real
## build passes a heightmap and bakes real elevation — same interface.
class_name ProcMeshCache
extends Node

var _cache: Dictionary = {}   # Vector2i -> ArrayMesh
var _grid := 16              # cells per chunk side (set by LOD density)
var _hits := 0
var _misses := 0


func set_lod_density(level: int) -> void:
	match level:
		0: _grid = 16
		1: _grid = 24
		_: _grid = 32


## Return a cached mesh for the chunk, or build + cache it. `hm` may be null (flat build).
func get_or_build(chunk_key: Vector2i, hm: Image) -> ArrayMesh:
	if _cache.has(chunk_key):
		_hits += 1
		return _cache[chunk_key]
	_misses += 1
	var mesh := _build(chunk_key, hm)
	_cache[chunk_key] = mesh
	return mesh


func _build(chunk_key: Vector2i, hm: Image) -> ArrayMesh:
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.35, 0.5, 0.3, 1.0)
	st.set_material(mat)
	var cols := _grid
	var rows := _grid
	var cell := 8.0   # world units per cell; chunk world size implied by CHUNK_SIZE/128
	for z in range(rows):
		for x in range(cols):
			var h := 0.0
			if hm != null and hm.get_width() > x and hm.get_height() > z:
				h = hm.get_pixel(x, z).r
			st.add_vertex(Vector3(float(x) * cell, h * 4.0, float(z) * cell))
	for z in range(rows - 1):
		for x in range(cols - 1):
			var i0: int = z * cols + x
			var i1: int = z * cols + (x + 1)
			var i2: int = (z + 1) * cols + x
			var i3: int = (z + 1) * cols + (x + 1)
			st.add_index(i0); st.add_index(i2); st.add_index(i1)
			st.add_index(i1); st.add_index(i2); st.add_index(i3)
	return st.commit()


## Triangle count for a single cached chunk mesh (index-buffer based).
func chunk_triangle_count() -> int:
	if _cache.is_empty():
		return 0
	var first: ArrayMesh = _cache.values()[0]
	var arr: Array = first.surface_get_arrays(0)
	if arr.is_empty():
		return 0
	var idx: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
	if idx.size() > 0:
		return idx.size() / 3
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	return verts.size() / 3


func hit_rate() -> float:
	var total := _hits + _misses
	return 0.0 if total == 0 else float(_hits) / float(total)


func cache_size() -> int:
	return _cache.size()
