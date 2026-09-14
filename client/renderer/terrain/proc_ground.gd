## TASK-035 · Renderer (2.5D)
## ProcGround: procedural ground mesh from a heightmap. No DCC 3D models (spec §5).
## Built once via SurfaceTool -> ArrayMesh; triangle count read back from the real mesh.
class_name ProcGround
extends Node3D

var _mesh: ArrayMesh = null
var _density := 1


## Build a single ArrayMesh covering the heightmap grid. One mesh => one draw call,
## regardless of grid cell count (the "1000 ground blocks" are cells within this mesh).
## A null/empty heightmap (e.g. headless where Image raster creation is unavailable)
## degrades to a flat grid so the pipeline still runs and is measurable.
func build_from_heightmap(hm: Image, tile_set: Texture2D) -> ArrayMesh:
	if hm == null or hm.get_width() == 0 or hm.get_height() == 0:
		return build_flat(32, 32, tile_set)
	return _build(hm.get_width(), hm.get_height(), tile_set, hm)


## Explicit flat ground (no heightmap). Used by headless tests/benchmarks and as a
## valid real-world case (flat terrain). Honest: no Image raster needed.
func build_flat(cols: int, rows: int, tile_set: Texture2D) -> ArrayMesh:
	return _build(cols, rows, tile_set, null)


func _build(cols: int, rows: int, tile_set: Texture2D, hm: Image) -> ArrayMesh:
	cols = maxi(2, cols)
	rows = maxi(2, rows)
	var st := SurfaceTool.new()
	st.begin(Mesh.PRIMITIVE_TRIANGLES)
	var mat := StandardMaterial3D.new()
	mat.albedo_color = Color(0.35, 0.5, 0.3, 1.0)
	if tile_set != null:
		mat.albedo_texture = tile_set
	st.set_material(mat)
	var cell := 2.0
	for z in range(rows):
		for x in range(cols):
			var h := 0.0
			if hm != null:
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
	_mesh = st.commit()
	return _mesh


func set_lod_density(level: int) -> void:
	_density = level


## Real triangle count: prefer the index buffer (SurfaceTool commits an indexed
## mesh); fall back to vertex count for non-indexed surfaces.
func triangle_count() -> int:
	if _mesh == null:
		return 0
	var arr: Array = _mesh.surface_get_arrays(0)
	if arr.is_empty():
		return 0
	var idx_arr: PackedInt32Array = arr[Mesh.ARRAY_INDEX]
	if idx_arr.size() > 0:
		return idx_arr.size() / 3
	var verts: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
	return verts.size() / 3


func mesh() -> ArrayMesh:
	return _mesh
