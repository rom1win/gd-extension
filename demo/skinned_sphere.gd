extends MeshInstance3D
## Test-scene tool (like head_shake.gd) -- NOT product.
##
## demo/blender_hair_test.tscn's "Emitter" is a plain SphereMesh with no bone
## weights and no Skin resource, so HairBinding::BindRootsToGodotMesh
## (src/HairBinding.cpp) has nothing to read and the .ghair loader falls back
## to uniform bone skinning. This script converts the sphere into the
## simplest possible "skinned" mesh -- every vertex bound 100% to bone 0 --
## purely so the bind_body_path root-binding path has real bone weights + a
## Skin resource to exercise end to end.
##
## Runs in _ready(); TressFXCharacter defers its asset loading with
## call_deferred("load_all_assets") (src/tressfx_character.cpp), which fires
## only after every node's _ready() in the tree has run, so sibling _ready()
## order between Emitter and TressFXCharacter does not matter here.

func _ready() -> void:
	var source_mesh: Mesh = mesh
	if source_mesh == null or source_mesh.get_surface_count() == 0:
		return

	var arrays: Array = source_mesh.surface_get_arrays(0)
	var vertex_count: int = (arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array).size()

	var bones := PackedInt32Array()
	var weights := PackedFloat32Array()
	bones.resize(vertex_count * 4)
	weights.resize(vertex_count * 4)
	for i in vertex_count:
		# 4 influences/vertex, all rigidly bound to bind index 0 ("root").
		bones[i * 4 + 0] = 0
		weights[i * 4 + 0] = 1.0
		# bones[i*4+1..3] default to 0, weights default to 0.0 -- fine.

	arrays[Mesh.ARRAY_BONES] = bones
	arrays[Mesh.ARRAY_WEIGHTS] = weights

	var array_mesh := ArrayMesh.new()
	array_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	mesh = array_mesh

	# Skin bind name must match the Skeleton3D bone name ("root" in this
	# scene) -- HairBinding.cpp resolves bind index -> skeleton bone by name
	# via Skeleton3D::find_bone.
	var mesh_skin := Skin.new()
	mesh_skin.set_bind_count(1)
	mesh_skin.set_bind_name(0, "root")
	mesh_skin.set_bind_pose(0, Transform3D.IDENTITY)
	skin = mesh_skin
