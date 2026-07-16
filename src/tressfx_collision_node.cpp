#include "tressfx_collision_node.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/core/property_info.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/variant/array.hpp>
#include <vector>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/sphere_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include "tressfx_character.h"

using namespace godot;

void TressFXCollisionNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_tfx_collision_asset"), &TressFXCollisionNode::load_tfx_collision_asset);
    ClassDB::bind_method(D_METHOD("find_tfxmesh_files"), &TressFXCollisionNode::find_tfxmesh_files);

    // Bind setters/getters
    ClassDB::bind_method(D_METHOD("set_tfx_mesh_file", "p"), &TressFXCollisionNode::set_tfx_mesh_file);
    ClassDB::bind_method(D_METHOD("get_tfx_mesh_file"), &TressFXCollisionNode::get_tfx_mesh_file);

    ClassDB::bind_method(D_METHOD("set_num_cells_in_x", "p"), &TressFXCollisionNode::set_num_cells_in_x);
    ClassDB::bind_method(D_METHOD("get_num_cells_in_x"), &TressFXCollisionNode::get_num_cells_in_x);

    ClassDB::bind_method(D_METHOD("set_collision_margin", "p"), &TressFXCollisionNode::set_collision_margin);
    ClassDB::bind_method(D_METHOD("get_collision_margin"), &TressFXCollisionNode::get_collision_margin);

    ClassDB::bind_method(D_METHOD("set_mesh", "p"), &TressFXCollisionNode::set_mesh);
    ClassDB::bind_method(D_METHOD("get_mesh"), &TressFXCollisionNode::get_mesh);

    ClassDB::bind_method(D_METHOD("set_follow_bone", "p"), &TressFXCollisionNode::set_follow_bone);
    ClassDB::bind_method(D_METHOD("get_follow_bone"), &TressFXCollisionNode::get_follow_bone);

    ClassDB::bind_method(D_METHOD("set_skeleton_node_path", "p"), &TressFXCollisionNode::set_skeleton_node_path);
    ClassDB::bind_method(D_METHOD("get_skeleton_node_path"), &TressFXCollisionNode::get_skeleton_node_path);

    ClassDB::bind_method(D_METHOD("set_capsule_enabled", "p"), &TressFXCollisionNode::set_capsule_enabled);
    ClassDB::bind_method(D_METHOD("get_capsule_enabled"), &TressFXCollisionNode::get_capsule_enabled);
    ClassDB::bind_method(D_METHOD("set_capsule_point_a", "p"), &TressFXCollisionNode::set_capsule_point_a);
    ClassDB::bind_method(D_METHOD("get_capsule_point_a"), &TressFXCollisionNode::get_capsule_point_a);
    ClassDB::bind_method(D_METHOD("set_capsule_point_b", "p"), &TressFXCollisionNode::set_capsule_point_b);
    ClassDB::bind_method(D_METHOD("get_capsule_point_b"), &TressFXCollisionNode::get_capsule_point_b);
    ClassDB::bind_method(D_METHOD("set_capsule_radius_a", "p"), &TressFXCollisionNode::set_capsule_radius_a);
    ClassDB::bind_method(D_METHOD("get_capsule_radius_a"), &TressFXCollisionNode::get_capsule_radius_a);
    ClassDB::bind_method(D_METHOD("set_capsule_radius_b", "p"), &TressFXCollisionNode::set_capsule_radius_b);
    ClassDB::bind_method(D_METHOD("get_capsule_radius_b"), &TressFXCollisionNode::get_capsule_radius_b);
    ClassDB::bind_method(D_METHOD("set_show_debug_capsule", "p"), &TressFXCollisionNode::set_show_debug_capsule);
    ClassDB::bind_method(D_METHOD("get_show_debug_capsule"), &TressFXCollisionNode::get_show_debug_capsule);

    ADD_PROPERTY(PropertyInfo(Variant::STRING, "tfx_mesh_file", PROPERTY_HINT_FILE, "*.tfxmesh"), "set_tfx_mesh_file", "get_tfx_mesh_file");
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "skeleton_node_path"), "set_skeleton_node_path", "get_skeleton_node_path");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "numCellsInXAxis"), "set_num_cells_in_x", "get_num_cells_in_x");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collisionMargin"), "set_collision_margin", "get_collision_margin");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh"), "set_mesh", "get_mesh");
    // followBone is provided dynamically via _get_property_list so the inspector can present an up-to-date enum

    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "capsule_enabled"), "set_capsule_enabled", "get_capsule_enabled");
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "capsule_point_a"), "set_capsule_point_a", "get_capsule_point_a");
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "capsule_point_b"), "set_capsule_point_b", "get_capsule_point_b");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "capsule_radius_a", PROPERTY_HINT_RANGE, "0.001,1.0,0.001"), "set_capsule_radius_a", "get_capsule_radius_a");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "capsule_radius_b", PROPERTY_HINT_RANGE, "0.001,1.0,0.001"), "set_capsule_radius_b", "get_capsule_radius_b");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_debug_capsule"), "set_show_debug_capsule", "get_show_debug_capsule");
}

TressFXCollisionNode::TressFXCollisionNode() {
}

TressFXCollisionNode::~TressFXCollisionNode() {
}

void TressFXCollisionNode::_init() {
}

void TressFXCollisionNode::_ready() {
	if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
		return;
	}

    // Drives the show_debug_capsule authoring aid (see _process); never runs
    // under the editor (guarded again inside _process for safety).
    set_process(true);

    // Populate a minimal collision description and register it with parent character if present
    last_collision_description.name = String("collision");
    last_collision_description.tfx_mesh_file = tfx_mesh_file;
    last_collision_description.numCellsInXAxis = numCellsInXAxis;
    last_collision_description.collisionMargin = collisionMargin;
    last_collision_description.mesh = mesh;
    last_collision_description.followBone = followBone;
    // Store skeleton path relative to the character (not this node), so the character
    // can reliably resolve it later regardless of where this node sits in the tree.
    last_collision_description.skeleton_node_path = String();

    // Validate bone existence
    if (!skeleton_node_path.is_empty() && !followBone.is_empty()) {
        Node *n = get_node<Node>(skeleton_node_path);
        Skeleton3D *s = Object::cast_to<Skeleton3D>(n);
        if (s) {
            if (s->find_bone(followBone) == -1) {
                 UtilityFunctions::push_warning(String("TressFXCollisionNode: Bone '") + followBone + String("' not found in skeleton!"));
            }
        }
    }

    UtilityFunctions::print(String("TressFXCollisionNode::_ready() values: tfx_mesh_file='") + tfx_mesh_file + String("' numCellsInXAxis=") + String::num_int64(numCellsInXAxis) + String(" followBone='") + followBone + String("'"));

    Node *p = get_parent();
    while (p) {
        TressFXCharacter *character = Object::cast_to<TressFXCharacter>(p);
        if (character) {
            if (!skeleton_node_path.is_empty()) {
                Node *sn = get_node_or_null(skeleton_node_path);
                Skeleton3D *sk = Object::cast_to<Skeleton3D>(sn);
                if (sk) {
                    const NodePath rel = character->get_path_to(sk);
                    last_collision_description.skeleton_node_path = String(rel);
                    UtilityFunctions::print(
                        String("TressFXCollisionNode: resolved skeleton for character: node_path='") + String(skeleton_node_path) +
                        String("' character_path='") + String(rel) +
                        String("' bone_count=") + String::num_int64(sk->get_bone_count()));
                } else {
                    UtilityFunctions::push_warning(String("TressFXCollisionNode: skeleton_node_path set but is not a Skeleton3D: ") + String(skeleton_node_path));
                }
            } else {
                UtilityFunctions::print("TressFXCollisionNode: skeleton_node_path not set (collision will use character default skeleton if any)");
            }

            // Only register if the mesh file is set
            if (!last_collision_description.tfx_mesh_file.is_empty()) {
                character->register_collision_description(last_collision_description);
            } else {
                UtilityFunctions::print(String("TressFXCollisionNode: tfx_mesh_file empty; skipping registration."));
            }
            // Capsule authoring is independent of tfx_mesh_file (a node can be
            // capsule-only); the character dedupes by pointer so this and
            // register_to_character() both calling it is harmless.
            character->register_capsule_node(this);
            break;
        }
        p = p->get_parent();
    }
}

void TressFXCollisionNode::set_skeleton_node_path(const NodePath &p) {
    skeleton_node_path = p;
    // Notify the editor to refresh property list so followBone enum is updated
    notify_property_list_changed();
}

// Dynamic property list to expose `followBone` as an enum of bones when a skeleton is assigned
void TressFXCollisionNode::_get_property_list(List<PropertyInfo> *p_list) const {
    // First add any default properties by not interfering (we only add followBone here)
    Array bones = const_cast<TressFXCollisionNode *>(this)->find_bones();

    String hint;
    for (int i = 0; i < (int)bones.size(); ++i) {
        if (i > 0) hint += String(",");
        hint += bones[i];
    }

    p_list->push_back(PropertyInfo(Variant::STRING, "followBone", PROPERTY_HINT_ENUM, hint));
}

bool TressFXCollisionNode::_set(const StringName &p_name, const Variant &p_value) {
    String name = p_name;
    if (name == "followBone") {
        if (p_value.get_type() == Variant::STRING) {
            followBone = p_value;
            return true;
        }
        return false;
    }
    return false;
}

bool TressFXCollisionNode::_get(const StringName &p_name, Variant &r_ret) const {
    String name = p_name;
    if (name == "followBone") {
        r_ret = followBone;
        return true;
    }
    return false;
}

void TressFXCollisionNode::load_tfx_collision_asset() {
    last_collision_description.name = String("collision");
    last_collision_description.tfx_mesh_file = tfx_mesh_file;
    last_collision_description.numCellsInXAxis = numCellsInXAxis;
    last_collision_description.collisionMargin = collisionMargin;
    last_collision_description.mesh = mesh;
    last_collision_description.followBone = followBone;
    // Keep this as node-relative; register_to_character will rewrite to character-relative.
    last_collision_description.skeleton_node_path = String(skeleton_node_path);

    UtilityFunctions::print(String("TressFXCollisionNode: created collision description: ") + last_collision_description.tfx_mesh_file);
}

void TressFXCollisionNode::register_to_character(TressFXCharacter *character) {
    load_tfx_collision_asset();
    if (character) {
        if (!skeleton_node_path.is_empty()) {
            Node *sn = get_node_or_null(skeleton_node_path);
            Skeleton3D *sk = Object::cast_to<Skeleton3D>(sn);
            if (sk) {
                const NodePath rel = character->get_path_to(sk);
                last_collision_description.skeleton_node_path = String(rel);
                UtilityFunctions::print(
                    String("TressFXCollisionNode: register_to_character resolved skeleton: node_path='") + String(skeleton_node_path) +
                    String("' character_path='") + String(rel) +
                    String("' bone_count=") + String::num_int64(sk->get_bone_count()));
            } else {
                UtilityFunctions::push_warning(String("TressFXCollisionNode: skeleton_node_path set but is not a Skeleton3D: ") + String(skeleton_node_path));
            }
        } else {
            UtilityFunctions::print("TressFXCollisionNode: register_to_character skeleton_node_path not set");
        }
        if (!last_collision_description.tfx_mesh_file.is_empty()) {
            character->register_collision_description(last_collision_description);
        } else {
            UtilityFunctions::print(String("TressFXCollisionNode::register_to_character: tfx_mesh_file empty; skipping registration."));
        }
        character->register_capsule_node(this);
    }
}

Array TressFXCollisionNode::find_tfxmesh_files() {
    Array results;

    std::vector<String> dirs;
    dirs.push_back(String("res://"));

    while (!dirs.empty()) {
        String dirpath = dirs.back();
        dirs.pop_back();

        Ref<DirAccess> da = DirAccess::open(dirpath);
        if (da.is_null()) {
            continue;
        }

        da->list_dir_begin();
        while (true) {
            String name = da->get_next();
            if (name == String()) break;
            if (name == String(".") || name == String("..")) continue;

            String full = dirpath;
            if (!full.ends_with("/"))
                full += String("/");
            full += name;

            if (da->current_is_dir()) {
                dirs.push_back(full);
            } else {
                String lname = name.to_lower();
                if (lname.ends_with(".tfxmesh")) {
                    results.push_back(full);
                }
            }
        }
        da->list_dir_end();
    }

    return results;
}

Array TressFXCollisionNode::find_bones() {
    Array results;

    if (skeleton_node_path == NodePath()) {
        return results;
    }

    Node *n = get_node<Node>(skeleton_node_path);
    if (!n) {
        UtilityFunctions::print(String("TressFXCollisionNode::find_bones: skeleton node not found: ") + String(skeleton_node_path));
        return results;
    }

    Skeleton3D *s = Object::cast_to<Skeleton3D>(n);
    if (!s) {
        UtilityFunctions::print(String("TressFXCollisionNode::find_bones: node is not a Skeleton3D: ") + String(skeleton_node_path));
        return results;
    }

    int32_t count = s->get_bone_count();
    for (int i = 0; i < count; ++i) {
        String name = s->get_bone_name(i);
        results.push_back(name);
    }

    return results;
}

bool TressFXCollisionNode::get_capsule_data(Vector3 &out_a, float &out_radius_a,
                                             Vector3 &out_b, float &out_radius_b) const {
    if (!capsule_enabled) {
        return false;
    }
    if (skeleton_node_path.is_empty() || followBone.is_empty()) {
        return false;
    }

    Node *n = get_node_or_null(skeleton_node_path);
    Skeleton3D *sk = Object::cast_to<Skeleton3D>(n);
    if (!sk) {
        return false;
    }
    const int32_t bone_idx = sk->find_bone(followBone);
    if (bone_idx < 0) {
        return false;
    }

    // Same space the sim's own vertex positions end up in after skinning
    // (see TressFXCharacter::update_gpu_debug_hair_lines_3d_transform) -- do
    // NOT also multiply by the skeleton's global_transform here, that only
    // applies at the final render mount.
    const Transform3D pose = sk->get_bone_global_pose(bone_idx);
    out_a = pose.xform(capsule_point_a);
    out_b = pose.xform(capsule_point_b);
    out_radius_a = capsule_radius_a;
    out_radius_b = capsule_radius_b;
    return true;
}

void TressFXCollisionNode::set_show_debug_capsule(bool p) {
    show_debug_capsule = p;
    if (!show_debug_capsule) {
        clear_debug_capsule_visual();
    }
}

void TressFXCollisionNode::_process(double delta) {
    if (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        return;
    }
    if (!show_debug_capsule) {
        return;
    }
    update_debug_capsule_visual();
}

void TressFXCollisionNode::ensure_debug_capsule_visual() {
    if (m_debug_sphere_a && m_debug_sphere_b) {
        return;
    }

    Ref<StandardMaterial3D> mat;
    mat.instantiate();
    mat->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
    mat->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
    mat->set_albedo(Color(1.0f, 0.2f, 0.9f, 0.35f));

    if (!m_debug_sphere_a) {
        m_debug_sphere_mesh_a.instantiate();
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_name("TressFXDebugCapsuleA");
        mi->set_mesh(m_debug_sphere_mesh_a);
        mi->set_material_override(mat);
        add_child(mi);
        m_debug_sphere_a = mi;
    }
    if (!m_debug_sphere_b) {
        m_debug_sphere_mesh_b.instantiate();
        MeshInstance3D *mi = memnew(MeshInstance3D);
        mi->set_name("TressFXDebugCapsuleB");
        mi->set_mesh(m_debug_sphere_mesh_b);
        mi->set_material_override(mat);
        add_child(mi);
        m_debug_sphere_b = mi;
    }
}

void TressFXCollisionNode::clear_debug_capsule_visual() {
    if (m_debug_sphere_a) {
        m_debug_sphere_a->queue_free();
        m_debug_sphere_a = nullptr;
        m_debug_sphere_mesh_a.unref();
        m_debug_sphere_applied_radius_a = -1.0f;
    }
    if (m_debug_sphere_b) {
        m_debug_sphere_b->queue_free();
        m_debug_sphere_b = nullptr;
        m_debug_sphere_mesh_b.unref();
        m_debug_sphere_applied_radius_b = -1.0f;
    }
}

void TressFXCollisionNode::update_debug_capsule_visual() {
    Vector3 a, b;
    float ra = 0.0f, rb = 0.0f;
    if (!get_capsule_data(a, ra, b, rb)) {
        clear_debug_capsule_visual();
        return;
    }

    Node *n = get_node_or_null(skeleton_node_path);
    Skeleton3D *sk = Object::cast_to<Skeleton3D>(n);
    if (!sk) {
        clear_debug_capsule_visual();
        return;
    }

    // get_capsule_data() returns endpoints in SKELETON MODEL SPACE (for the
    // sim); the debug spheres are ordinary world-space nodes, so mount them
    // via the skeleton's own global transform (same convention as the ribbon
    // render mount).
    const Transform3D skel_xform = sk->get_global_transform();
    const Vector3 world_a = skel_xform.xform(a);
    const Vector3 world_b = skel_xform.xform(b);

    ensure_debug_capsule_visual();

    m_debug_sphere_a->set_global_position(world_a);
    m_debug_sphere_b->set_global_position(world_b);

    // Root cause of the runtime crash on weaker/integrated GPUs (Intel Iris
    // Xe): PrimitiveMesh::set_radius/set_height unconditionally re-tessellate
    // the sphere and re-upload it to the RenderingServer. Calling them every
    // _process() tick regenerated+re-uploaded an unchanged mesh 60x/sec,
    // forever, on top of the hair-sim GPU load -- fine on a strong discrete
    // GPU, enough sustained churn to crash a weaker one after a few seconds.
    // Only touch the mesh when the authored radius actually changed (still
    // live-editable from the Remote inspector: next tick after a Set call
    // this differs from the cached value and re-applies once).
    if (m_debug_sphere_mesh_a.is_valid() && ra != m_debug_sphere_applied_radius_a) {
        m_debug_sphere_mesh_a->set_radius(ra);
        m_debug_sphere_mesh_a->set_height(ra * 2.0f);
        m_debug_sphere_applied_radius_a = ra;
    }
    if (m_debug_sphere_mesh_b.is_valid() && rb != m_debug_sphere_applied_radius_b) {
        m_debug_sphere_mesh_b->set_radius(rb);
        m_debug_sphere_mesh_b->set_height(rb * 2.0f);
        m_debug_sphere_applied_radius_b = rb;
    }
}
