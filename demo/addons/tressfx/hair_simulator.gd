extends RefCounted

## Phase 2 of the rewrite: the GDScript simulation host (replaces the C++
## Simulation/TressFXHairObject/EngineInterface plumbing).
##
## Owns GPU buffers and compute pipelines on the MAIN RenderingDevice and runs
## the TressFX kernel chain once per frame. All RD work happens on the render
## thread via RenderingServer.call_on_render_thread(); the main thread only
## packs parameter bytes (decision 4 in CLAUDE.md: no submit(), no sync(),
## readback only through async requests).
##
## Everything here follows NOTES.md: §2 UBO layout (_pack_sim_params is the one
## place that knows it), §3 buffers/bindings, §4 dispatch order, §6 defaults.

const THREAD_GROUP_SIZE := 64
const MAX_BONES := 128  # must match AMD_TRESSFX_MAX_NUM_BONES in the GLSL kernels (AUDIT F2)
const UBO_SIZE := 160 + MAX_BONES * 64  # 8352 bytes for 128 bones

const KERNEL_DIR := "res://shaders/glsl/"
# Dispatch order per TressFXSimulation.cpp (NOTES §4). "strand" kernels run one
# thread per guide strand, "vertex" kernels one thread per vertex.
const KERNELS := [
	{ "name": "IntegrationAndGlobalShapeConstraints", "level": "vertex" },
	{ "name": "CalculateStrandLevelData", "level": "strand" },
	{ "name": "VelocityShockPropagation", "level": "vertex" },
	{ "name": "LocalShapeConstraints", "level": "strand" },  # dispatched local_iterations times
	{ "name": "LengthConstriantsWindAndCollision", "level": "vertex" },
]

# ---- tunables (RatBoy defaults, NOTES §6) ----
var damping := 0.068
var local_stiffness := 0.908
var global_stiffness := 0.408
var global_range := 0.308
var gravity_magnitude := 0.09
var length_iterations := 3
var local_iterations := 3       # vps < 64 -> host repeats the kernel (NOTES §6)
var vsp_coeff := 0.758
var vsp_accel_threshold := 1.208
var clamp_position_delta := 20.0
var tip_separation := 1.0
var wind_velocity := Vector3.ZERO

# ---- asset-derived counts (set in setup()) ----
var _vps := 0
var _total_strands := 0
var _total_vertices := 0
var _follow_per_guide := 0
var _vertex_groups := 0
var _strand_groups := 0

# ---- CPU-side copies handed to the render thread at init ----
var _init_positions_bytes: PackedByteArray
var _init_tangents_bytes: PackedByteArray
var _rest_lengths_bytes: PackedByteArray
var _strand_types_bytes: PackedByteArray
var _follow_offsets_bytes: PackedByteArray
var _bone_skinning_bytes: PackedByteArray

# ---- render-thread state (touch ONLY inside _rt_* functions) ----
var _rd: RenderingDevice
var _shaders: Array[RID] = []
var _pipelines: Array[RID] = []
var _buffers: Dictionary = {}       # name -> RID
var _uniform_sets: Array = []       # per kernel: [set0_rid, set1_rid]
var _rt_ready := false
var _init_failed := false

var _identity_bones_bytes: PackedByteArray


func setup(asset) -> void:
	# Snapshot everything the render thread needs, by value (never share the
	# asset object across threads).
	_vps = asset.num_verts_per_strand
	_total_strands = asset.num_total_strands
	_total_vertices = asset.num_total_vertices
	_follow_per_guide = asset.num_follow_per_guide
	_vertex_groups = _total_vertices / THREAD_GROUP_SIZE
	_strand_groups = _total_strands / THREAD_GROUP_SIZE

	_init_positions_bytes = asset.positions.to_byte_array()
	_init_tangents_bytes = asset.tangents.to_byte_array()
	_rest_lengths_bytes = asset.rest_lengths.to_byte_array()
	_strand_types_bytes = asset.strand_types.to_byte_array()
	_follow_offsets_bytes = asset.follow_root_offsets.to_byte_array()
	_bone_skinning_bytes = asset.bone_skinning.to_byte_array()

	# Identity skinning matrices (Phase 2 runs without a skeleton; Phase 3
	# replaces this with the main-thread pose snapshot). Packing convention
	# per NOTES §1 — identity is identity in any convention.
	var id := PackedFloat32Array()
	id.resize(MAX_BONES * 16)
	for i in MAX_BONES:
		id[i * 16 + 0] = 1.0
		id[i * 16 + 5] = 1.0
		id[i * 16 + 10] = 1.0
		id[i * 16 + 15] = 1.0
	_identity_bones_bytes = id.to_byte_array()

	RenderingServer.call_on_render_thread(_rt_initialize)


func is_ready() -> bool:
	return _rt_ready


func failed() -> bool:
	return _init_failed


## Main-thread entry: advance the simulation one step.
## reset=true replays the "teleport reset" the C++ does on the first two frames.
func simulate(dt: float, frame: int, reset: bool, bones_bytes: PackedByteArray = PackedByteArray()) -> void:
	var params := _pack_sim_params(dt, frame, reset, bones_bytes)
	RenderingServer.call_on_render_thread(_rt_simulate.bind(params))


## Main-thread entry: ask for the current positions buffer; `callback` receives
## a PackedByteArray (float4 per vertex) later, on the render thread — hop back
## to the main thread (call_deferred) before touching any scene node.
func request_positions(callback: Callable) -> void:
	RenderingServer.call_on_render_thread(_rt_request_positions.bind(callback))


## Main-thread entry: free all GPU objects. Call from _exit_tree.
func release() -> void:
	RenderingServer.call_on_render_thread(_rt_release)


# ---------------------------------------------------------------------------
# std140 parameter block packing — THE one place that knows the UBO layout
# (NOTES §2; must match TressFXSimulationParams and every kernel's cb block).
# ---------------------------------------------------------------------------
func _pack_sim_params(dt: float, frame: int, reset: bool, bones_bytes: PackedByteArray) -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(160)

	# g_Wind..g_Wind3 (offsets 0..63): four corners of a 40-degree cone around
	# the wind direction, magnitude pulsed over frames — the exact
	# TressFXHairObject::SetWind behavior. All zero when wind is off.
	var mag := wind_velocity.length()
	if mag > 0.0001:
		var wm := mag * (pow(sin(frame * 0.01), 2.0) + 0.5)
		var dir := wind_velocity / mag
		var rot_from_x := Quaternion(Vector3(1, 0, 0), dir)
		var cone := deg_to_rad(40.0)
		var axes := [Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, 0, 1), Vector3(0, 0, -1)]
		for i in 4:
			var corner := (rot_from_x * Quaternion(axes[i], cone)) * Vector3(1, 0, 0) * wm
			b.encode_float(i * 16 + 0, corner.x)
			b.encode_float(i * 16 + 4, corner.y)
			b.encode_float(i * 16 + 8, corner.z)
			# w unused (0)

	# g_Shape (64): damping, local stiffness, global stiffness, global range
	b.encode_float(64, damping)
	b.encode_float(68, local_stiffness)
	b.encode_float(72, global_stiffness)
	b.encode_float(76, global_range)

	# g_GravTimeTip (80): gravity, dt, tip separation
	b.encode_float(80, gravity_magnitude)
	b.encode_float(84, dt)
	b.encode_float(88, tip_separation)

	# g_SimInts (96): length iterations, local iterations (1: host repeats the
	# kernel instead because vps < 64 — NOTES §6), collision off
	b.encode_s32(96, length_iterations)
	b.encode_s32(100, 1)
	b.encode_s32(104, 0)

	# g_Counts (112): strands per thread group, follow per guide, vps
	b.encode_s32(112, THREAD_GROUP_SIZE / _vps)
	b.encode_s32(116, _follow_per_guide)
	b.encode_s32(120, _vps)

	# g_VSP (128)
	b.encode_float(128, vsp_coeff)
	b.encode_float(132, vsp_accel_threshold)

	# scalars (144): reset flag, velocity clamp, two pads
	b.encode_float(144, 1.0 if reset else 0.0)
	b.encode_float(148, clamp_position_delta)

	# g_BoneSkinningMatrix[128] (160): caller-provided pose snapshot or identity.
	if bones_bytes.size() == _identity_bones_bytes.size():
		b.append_array(bones_bytes)
	else:
		b.append_array(_identity_bones_bytes)
	return b


# ---------------------------------------------------------------------------
# Render-thread side
# ---------------------------------------------------------------------------
func _rt_initialize() -> void:
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		push_error("HairSimulator: no main RenderingDevice (Forward+ renderer required)")
		_init_failed = true
		return

	# Compile the kernels from source — same runtime path the C++ demo uses,
	# so both read the identical .comp.glsl files during the side-by-side.
	for k in KERNELS:
		var path: String = KERNEL_DIR + "TressFXSimulation." + k["name"] + ".comp.glsl"
		var src_text := FileAccess.get_file_as_string(path)
		if src_text.is_empty():
			push_error("HairSimulator: missing kernel source " + path)
			_init_failed = true
			return
		var src := RDShaderSource.new()
		src.set_stage_source(RenderingDevice.SHADER_STAGE_COMPUTE, src_text)
		var spirv := _rd.shader_compile_spirv_from_source(src)
		var err := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)
		if err != "":
			push_error("HairSimulator: kernel compile FAILED " + k["name"] + ":\n" + err)
			_init_failed = true
			return
		var shader := _rd.shader_create_from_spirv(spirv, "tressfx_gd_" + k["name"])
		var pipeline := _rd.compute_pipeline_create(shader)
		_shaders.append(shader)
		_pipelines.append(pipeline)
		print("[TressFX GD] kernel compiled OK: ", k["name"])

	# Buffers (NOTES §3). Set 1 position buffers all start at the rest pose.
	_buffers["initial_positions"] = _rd.storage_buffer_create(_init_positions_bytes.size(), _init_positions_bytes)
	_buffers["rest_lengths"] = _rd.storage_buffer_create(_rest_lengths_bytes.size(), _rest_lengths_bytes)
	_buffers["strand_types"] = _rd.storage_buffer_create(_strand_types_bytes.size(), _strand_types_bytes)
	_buffers["follow_offsets"] = _rd.storage_buffer_create(_follow_offsets_bytes.size(), _follow_offsets_bytes)
	_buffers["bone_skinning"] = _rd.storage_buffer_create(_bone_skinning_bytes.size(), _bone_skinning_bytes)
	_buffers["positions"] = _rd.storage_buffer_create(_init_positions_bytes.size(), _init_positions_bytes)
	_buffers["positions_prev"] = _rd.storage_buffer_create(_init_positions_bytes.size(), _init_positions_bytes)
	_buffers["positions_prev_prev"] = _rd.storage_buffer_create(_init_positions_bytes.size(), _init_positions_bytes)
	_buffers["tangents"] = _rd.storage_buffer_create(_init_tangents_bytes.size(), _init_tangents_bytes)
	var strand_level := PackedByteArray()
	strand_level.resize(_total_strands * 48)  # 3 x vec4 per strand, zero-filled
	_buffers["strand_level_data"] = _rd.storage_buffer_create(strand_level.size(), strand_level)
	_buffers["sim_params"] = _rd.uniform_buffer_create(UBO_SIZE)

	# Uniform sets. Godot ties a uniform set to a shader RID, so build one pair
	# per kernel (they all declare identical set layouts).
	for i in KERNELS.size():
		var set0 := _rd.uniform_set_create(_make_set0_uniforms(), _shaders[i], 0)
		var set1 := _rd.uniform_set_create(_make_set1_uniforms(), _shaders[i], 1)
		_uniform_sets.append([set0, set1])

	_rt_ready = true
	print("[TressFX GD] simulator initialized on main RD: strands=", _total_strands,
		" vertices=", _total_vertices, " vertex_groups=", _vertex_groups,
		" strand_groups=", _strand_groups)


func _make_set0_uniforms() -> Array:
	var u := []
	u.append(_storage_uniform(4, _buffers["initial_positions"]))
	u.append(_storage_uniform(5, _buffers["rest_lengths"]))
	u.append(_storage_uniform(6, _buffers["strand_types"]))
	u.append(_storage_uniform(7, _buffers["follow_offsets"]))
	u.append(_storage_uniform(12, _buffers["bone_skinning"]))
	var ubo := RDUniform.new()
	ubo.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
	ubo.binding = 13
	ubo.add_id(_buffers["sim_params"])
	u.append(ubo)
	return u


func _make_set1_uniforms() -> Array:
	var u := []
	u.append(_storage_uniform(0, _buffers["positions"]))
	u.append(_storage_uniform(1, _buffers["positions_prev"]))
	u.append(_storage_uniform(2, _buffers["positions_prev_prev"]))
	u.append(_storage_uniform(3, _buffers["tangents"]))
	u.append(_storage_uniform(4, _buffers["strand_level_data"]))
	return u


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = binding
	u.add_id(rid)
	return u


func _rt_simulate(params: PackedByteArray) -> void:
	if not _rt_ready:
		return
	# Refresh the parameter block, then run the kernel chain. Godot >= 4.3
	# tracks buffer dependencies between dispatches in a compute list, which
	# provides the UAV-barrier-after-each-kernel ordering AMD requires.
	_rd.buffer_update(_buffers["sim_params"], 0, params.size(), params)

	var cl := _rd.compute_list_begin()
	for i in KERNELS.size():
		var groups: int = _vertex_groups if KERNELS[i]["level"] == "vertex" else _strand_groups
		var repeats := local_iterations if KERNELS[i]["name"] == "LocalShapeConstraints" else 1
		_rd.compute_list_bind_compute_pipeline(cl, _pipelines[i])
		_rd.compute_list_bind_uniform_set(cl, _uniform_sets[i][0], 0)
		_rd.compute_list_bind_uniform_set(cl, _uniform_sets[i][1], 1)
		for r in repeats:
			_rd.compute_list_dispatch(cl, groups, 1, 1)
			_rd.compute_list_add_barrier(cl)
	_rd.compute_list_end()
	# Deliberately no submit()/sync(): the engine owns the main-RD frame.


func _rt_request_positions(callback: Callable) -> void:
	if not _rt_ready:
		return
	_rd.buffer_get_data_async(_buffers["positions"], callback)


func _rt_release() -> void:
	_rt_ready = false
	if _rd == null:
		return
	for pair in _uniform_sets:
		for rid in pair:
			if rid.is_valid():
				_rd.free_rid(rid)
	for p in _pipelines:
		if p.is_valid():
			_rd.free_rid(p)
	for s in _shaders:
		if s.is_valid():
			_rd.free_rid(s)
	for name in _buffers:
		if _buffers[name].is_valid():
			_rd.free_rid(_buffers[name])
	_uniform_sets.clear()
	_pipelines.clear()
	_shaders.clear()
	_buffers.clear()
