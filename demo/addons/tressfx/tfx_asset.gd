@tool
class_name HairAsset
extends Resource

## GDScript port of TressFX 4.1 asset loading (Phase 1 of the rewrite).
##
## Replaces the C++ TressFXAsset pipeline: LoadHairData -> GenerateFollowHairs ->
## ProcessAsset -> LoadBoneData. The binary formats and every padding/interleaving
## rule are documented in NOTES.md §5 and were verified against the C++ loader
## (thirdparty/tressfx/src/TressFX/TressFXAsset.cpp) and the RatBoy files.
##
## Layout reminder (all counts follow AMD's conventions):
## - The .tfx file contains only GUIDE strands. Guide count is padded UP to the
##   next multiple of 64 (always adding a full 64 even if already a multiple);
##   padded strands duplicate the last real strand.
## - After follow-hair generation the strand buffers are INTERLEAVED:
##   guide g lives at strand slot g*(follow_per_guide+1), its follow hairs in the
##   next follow_per_guide slots.
## - Every vertex is a float4; w carries movability (0 = pinned to head, 1 = free).
##   In RatBoy the first two vertices of each strand are pinned.

const THREAD_GROUP_SIZE := 64

# ---- counts ----
@export var num_guide_strands: int = 0        # padded guide count (multiple of 64)
@export var num_verts_per_strand: int = 0     # 4..64, must divide 64
@export var num_follow_per_guide: int = 0
@export var num_total_strands: int = 0        # guides * (follow+1), interleaved
@export var num_total_vertices: int = 0
@export var tip_separation: float = 0.0

# ---- per-vertex data (float4 streams: 4 floats per vertex) ----
@export var positions: PackedFloat32Array = PackedFloat32Array()      # xyzw, w = movability
@export var tangents: PackedFloat32Array = PackedFloat32Array()       # xyzw, last vertex of each strand stays zero (AMD quirk)
@export var rest_lengths: PackedFloat32Array = PackedFloat32Array()   # 1 float per vertex, last of each strand = 0

# ---- per-strand data ----
@export var follow_root_offsets: PackedFloat32Array = PackedFloat32Array() # float4 per strand slot; w = guide slot index
@export var strand_uv: PackedFloat32Array = PackedFloat32Array()           # float2 per strand slot
@export var strand_types: PackedInt32Array = PackedInt32Array()            # all zeros (unused upstream)

# ---- skinning data (.tfxbone) ----
# 8 floats per strand slot: 4 bone indices (stored as floats, FILE-local indices,
# not engine bone ids — the name->Skeleton3D mapping happens in Phase 3) then 4 weights.
# Valid entries exist only at guide slots (g*(follow+1)); follow slots stay zero,
# exactly like the C++ buffer the kernels read.
@export var bone_skinning: PackedFloat32Array = PackedFloat32Array()
@export var bone_names: PackedStringArray = PackedStringArray()


# Instance method on purpose (not a static factory): referencing the class by
# its registered name inside its own static functions breaks in headless
# `godot -s` runs, where the global class registry is not loaded.
func load_from_files(tfx_path: String, tfxbone_path: String = "",
		follow_per_guide: int = 0, p_tip_separation: float = 0.0,
		max_radius_around_guide: float = 0.0) -> bool:
	if not _load_hair_data(tfx_path):
		push_error("HairAsset: failed to load " + tfx_path)
		return false
	_generate_follow_hairs(follow_per_guide, p_tip_separation, max_radius_around_guide)
	_process_asset()
	if tfxbone_path != "":
		if not _load_bone_data(tfxbone_path):
			push_error("HairAsset: failed to load " + tfxbone_path)
			return false
	return true


# ---------------------------------------------------------------------------
# .tfx  (spec: NOTES.md §5, TressFXAsset::LoadHairData)
# Header: float version; u32 numHairStrands; u32 numVerticesPerStrand;
#         u32 offsetVertexPosition; u32 offsetStrandUV; u32 offsetVertexUV;
#         u32 offsetStrandThickness; u32 offsetVertexColor; u32 reserved[32]
# ---------------------------------------------------------------------------
func _load_hair_data(path: String) -> bool:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return false

	var version := f.get_float()
	var strands_in_file := f.get_32()
	num_verts_per_strand = f.get_32()
	var offset_vertex_position := f.get_32()
	var offset_strand_uv := f.get_32()

	if version < 4.0:
		push_error("HairAsset: unsupported .tfx version %f" % version)
		return false
	if num_verts_per_strand <= 2 or num_verts_per_strand > THREAD_GROUP_SIZE \
			or THREAD_GROUP_SIZE % num_verts_per_strand != 0:
		push_error("HairAsset: invalid vertices-per-strand %d (must be 4/8/16/32/64)" % num_verts_per_strand)
		return false

	# AMD padding rule: ALWAYS add a full 64-group, even when already a multiple.
	num_guide_strands = (strands_in_file - strands_in_file % THREAD_GROUP_SIZE) + THREAD_GROUP_SIZE
	num_follow_per_guide = 0
	num_total_strands = num_guide_strands
	num_total_vertices = num_guide_strands * num_verts_per_strand

	# Positions: float4 per vertex, only the real file strands, then pad by
	# duplicating the last real strand.
	f.seek(offset_vertex_position)
	var file_floats := f.get_buffer(strands_in_file * num_verts_per_strand * 16).to_float32_array()
	positions = PackedFloat32Array()
	positions.resize(num_total_vertices * 4)
	for i in file_floats.size():
		positions[i] = file_floats[i]
	var last_strand_base := (strands_in_file - 1) * num_verts_per_strand * 4
	for s in range(strands_in_file, num_guide_strands):
		var dst := s * num_verts_per_strand * 4
		for k in num_verts_per_strand * 4:
			positions[dst + k] = positions[last_strand_base + k]

	# Strand UVs: float2 per strand, padded the same way (C++ reads them
	# unconditionally at offsetStrandUV).
	strand_uv = PackedFloat32Array()
	strand_uv.resize(num_total_strands * 2)
	if offset_strand_uv > 0:
		f.seek(offset_strand_uv)
		var uv_floats := f.get_buffer(strands_in_file * 8).to_float32_array()
		for i in uv_floats.size():
			strand_uv[i] = uv_floats[i]
		for s in range(strands_in_file, num_guide_strands):
			strand_uv[s * 2] = strand_uv[(strands_in_file - 1) * 2]
			strand_uv[s * 2 + 1] = strand_uv[(strands_in_file - 1) * 2 + 1]

	# Follow-root offsets start zero-filled (LoadHairData does memset 0).
	follow_root_offsets = PackedFloat32Array()
	follow_root_offsets.resize(num_total_strands * 4)
	return true


# ---------------------------------------------------------------------------
# Follow hairs (TressFXAsset::GenerateFollowHairs): interleave guide+follow
# strands, offset each follow root randomly within max_radius in the plane
# perpendicular to the root segment, widen toward the tip.
# NOTE: with max_radius == 0 (the current demo default) offsets are exactly
# zero and the result is deterministic. With max_radius > 0 the C++ uses
# rand(), so values would not match the C++ loader bit-for-bit — that mode is
# for the rendering-side expansion later, not for Gate 1 comparison.
# ---------------------------------------------------------------------------
func _generate_follow_hairs(p_follow_per_guide: int, p_tip_separation: float, max_radius: float) -> void:
	num_follow_per_guide = p_follow_per_guide
	tip_separation = p_tip_separation
	if p_follow_per_guide <= 0:
		return

	var stride := p_follow_per_guide + 1
	num_total_strands = num_guide_strands * stride
	num_total_vertices = num_total_strands * num_verts_per_strand

	var guide_positions := positions
	var guide_uv := strand_uv

	positions = PackedFloat32Array()
	positions.resize(num_total_vertices * 4)
	strand_uv = PackedFloat32Array()
	strand_uv.resize(num_total_strands * 2)
	follow_root_offsets = PackedFloat32Array()
	follow_root_offsets.resize(num_total_strands * 4)

	var vps := num_verts_per_strand
	for g in num_guide_strands:
		var guide_slot := g * stride
		var src := g * vps * 4
		var dst := guide_slot * vps * 4
		for k in vps * 4:
			positions[dst + k] = guide_positions[src + k]
		strand_uv[guide_slot * 2] = guide_uv[g * 2]
		strand_uv[guide_slot * 2 + 1] = guide_uv[g * 2 + 1]
		# Guide slot: zero offset, w records its own slot index (AMD convention).
		follow_root_offsets[guide_slot * 4 + 3] = float(guide_slot)

		# Two unit vectors perpendicular to the root segment (only needed when
		# max_radius > 0; the zero-radius path keeps offsets at exactly 0).
		var offset_basis_u := Vector3.ZERO
		var offset_basis_v := Vector3.ZERO
		if max_radius > 0.0:
			var root := Vector3(guide_positions[src], guide_positions[src + 1], guide_positions[src + 2])
			var next := Vector3(guide_positions[src + 4], guide_positions[src + 5], guide_positions[src + 6])
			var dir := (next - root).normalized()
			offset_basis_u = dir.cross(Vector3.UP)
			if offset_basis_u.length_squared() < 1e-8:
				offset_basis_u = dir.cross(Vector3.RIGHT)
			offset_basis_u = offset_basis_u.normalized()
			offset_basis_v = dir.cross(offset_basis_u).normalized()

		for j in p_follow_per_guide:
			var follow_slot := guide_slot + j + 1
			var fdst := follow_slot * vps * 4
			var offset := Vector3.ZERO
			if max_radius > 0.0:
				offset = randf_range(-max_radius, max_radius) * offset_basis_u \
					+ randf_range(-max_radius, max_radius) * offset_basis_v
			follow_root_offsets[follow_slot * 4] = offset.x
			follow_root_offsets[follow_slot * 4 + 1] = offset.y
			follow_root_offsets[follow_slot * 4 + 2] = offset.z
			follow_root_offsets[follow_slot * 4 + 3] = float(guide_slot)
			strand_uv[follow_slot * 2] = strand_uv[guide_slot * 2]
			strand_uv[follow_slot * 2 + 1] = strand_uv[guide_slot * 2 + 1]
			for k in vps:
				var factor := tip_separation * (float(k) / float(vps)) + 1.0
				positions[fdst + k * 4] = positions[dst + k * 4] + offset.x * factor
				positions[fdst + k * 4 + 1] = positions[dst + k * 4 + 1] + offset.y * factor
				positions[fdst + k * 4 + 2] = positions[dst + k * 4 + 2] + offset.z * factor
				positions[fdst + k * 4 + 3] = positions[dst + k * 4 + 3]  # copy movability


# ---------------------------------------------------------------------------
# Derived data (TressFXAsset::ProcessAsset): tangents, rest lengths, types.
# ---------------------------------------------------------------------------
func _process_asset() -> void:
	var vps := num_verts_per_strand

	strand_types = PackedInt32Array()
	strand_types.resize(num_total_strands)  # zero-filled: strand types are unused upstream

	# Rest lengths: distance to the next vertex; the strand's last vertex gets 0.
	rest_lengths = PackedFloat32Array()
	rest_lengths.resize(num_total_vertices)
	for s in num_total_strands:
		var base := s * vps
		for j in vps - 1:
			var a := (base + j) * 4
			var b := (base + j + 1) * 4
			var dx := positions[a] - positions[b]
			var dy := positions[a + 1] - positions[b + 1]
			var dz := positions[a + 2] - positions[b + 2]
			rest_lengths[base + j] = sqrt(dx * dx + dy * dy + dz * dz)
		rest_lengths[base + vps - 1] = 0.0

	# Tangents: first vertex = direction to next; middle vertices = average of the
	# two segment directions; LAST vertex is never written (stays zero) — that is
	# the exact upstream behavior, reproduce it.
	tangents = PackedFloat32Array()
	tangents.resize(num_total_vertices * 4)
	for s in num_total_strands:
		var base := s * vps
		var t0 := _vec(positions, base + 1) - _vec(positions, base)
		_put_vec(tangents, base, t0.normalized())
		for i in range(1, vps - 1):
			var pre := (_vec(positions, base + i) - _vec(positions, base + i - 1)).normalized()
			var nxt := (_vec(positions, base + i + 1) - _vec(positions, base + i)).normalized()
			_put_vec(tangents, base + i, (pre + nxt).normalized())


# ---------------------------------------------------------------------------
# .tfxbone (spec: NOTES.md §5 — the REAL layout the C++ loader reads, which is
# NOT the header struct in AMD's TressFXFileFormat.h):
#   i32 numBones
#   numBones * { i32 boneIndex; i32 nameLen(incl NUL); char name[nameLen] }
#   i32 numStrands
#   numStrands * { i32 strandIndex(unused); 4 * { i32 boneIndex; f32 weight } }
# Entries land at INTERLEAVED guide slots (i*(follow+1)) — so this must be
# called AFTER _generate_follow_hairs. Padding guides reuse the last entry.
# boneIndex -1 becomes 0 (its weight is 0 anyway).
# ---------------------------------------------------------------------------
func _load_bone_data(path: String) -> bool:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return false

	var num_bones := f.get_32()
	bone_names = PackedStringArray()
	bone_names.resize(num_bones)
	for i in num_bones:
		f.get_32()  # bone index within the file, unused (names are the key)
		var name_len := f.get_32()
		var raw := f.get_buffer(name_len)
		bone_names[i] = raw.slice(0, max(0, name_len - 1)).get_string_from_ascii()

	var strands_in_stream := f.get_32()
	if num_guide_strands < strands_in_stream:
		push_error("HairAsset: .tfxbone has more strands than .tfx (%d > %d)" % [strands_in_stream, num_guide_strands])
		return false

	var stride := num_follow_per_guide + 1
	bone_skinning = PackedFloat32Array()
	bone_skinning.resize(num_total_strands * 8)

	var last := PackedFloat32Array([0, 0, 0, 0, 0, 0, 0, 0])
	for i in strands_in_stream:
		f.get_32()  # per-strand index, unused by the C++ loader too
		var entry := PackedFloat32Array()
		entry.resize(8)
		for j in 4:
			var bone_index_u := f.get_32()
			var bone_index := -1 if bone_index_u == 0xFFFFFFFF else int(bone_index_u)
			entry[j] = float(maxi(bone_index, 0))
			entry[4 + j] = f.get_float()
		var slot := i * stride
		for k in 8:
			bone_skinning[slot * 8 + k] = entry[k]
		last = entry
	# Padding guides duplicate the last real strand's skin data (upstream behavior).
	for i in range(strands_in_stream, num_guide_strands):
		var slot := i * stride
		for k in 8:
			bone_skinning[slot * 8 + k] = last[k]
	return true


# ---- small helpers ----
static func _vec(arr: PackedFloat32Array, vertex_index: int) -> Vector3:
	var i := vertex_index * 4
	return Vector3(arr[i], arr[i + 1], arr[i + 2])


static func _put_vec(arr: PackedFloat32Array, vertex_index: int, v: Vector3) -> void:
	var i := vertex_index * 4
	arr[i] = v.x
	arr[i + 1] = v.y
	arr[i + 2] = v.z
	arr[i + 3] = 0.0
