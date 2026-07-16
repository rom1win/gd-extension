#ifndef TRESSFX_COLLISION_NODE_H
#define TRESSFX_COLLISION_NODE_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "tressfx_node.h" // reuse the collision description struct defined for hair node

using namespace godot;

namespace godot {
class MeshInstance3D;
class SphereMesh;
class StandardMaterial3D;
}

class TressFXCollisionNode : public Node3D {
    GDCLASS(TressFXCollisionNode, Node3D)

protected:
    static void _bind_methods();

public:
    TressFXCollisionNode();
    ~TressFXCollisionNode();

    void _init();
    void _ready();
    void _process(double delta);

    // Inspector accessors
    void set_tfx_mesh_file(const String &p) { tfx_mesh_file = p; }
    String get_tfx_mesh_file() const { return tfx_mesh_file; }

    void set_num_cells_in_x(int p) { numCellsInXAxis = p; }
    int get_num_cells_in_x() const { return numCellsInXAxis; }

    void set_collision_margin(float p) { collisionMargin = p; }
    float get_collision_margin() const { return collisionMargin; }

    void set_mesh(int p) { mesh = p; }
    int get_mesh() const { return mesh; }

    void set_follow_bone(const String &p) { followBone = p; }
    String get_follow_bone() const { return followBone; }

    void set_skeleton_node_path(const NodePath &p);
    NodePath get_skeleton_node_path() const { return skeleton_node_path; }

    // A3.2 subtask B: capsule authoring (offsets in the followBone's LOCAL space).
    void set_capsule_enabled(bool p) { capsule_enabled = p; }
    bool get_capsule_enabled() const { return capsule_enabled; }

    void set_capsule_point_a(const Vector3 &p) { capsule_point_a = p; }
    Vector3 get_capsule_point_a() const { return capsule_point_a; }

    void set_capsule_point_b(const Vector3 &p) { capsule_point_b = p; }
    Vector3 get_capsule_point_b() const { return capsule_point_b; }

    void set_capsule_radius_a(float p) { capsule_radius_a = p; }
    float get_capsule_radius_a() const { return capsule_radius_a; }

    void set_capsule_radius_b(float p) { capsule_radius_b = p; }
    float get_capsule_radius_b() const { return capsule_radius_b; }

    void set_show_debug_capsule(bool p);
    bool get_show_debug_capsule() const { return show_debug_capsule; }

    // Main-thread only: resolves the followBone's CURRENT pose in SKELETON
    // MODEL SPACE (Skeleton3D::get_bone_global_pose -- the same space the sim
    // outputs vertex positions in, see NOTES/A3.1) and transforms the
    // authored endpoints by it. Returns false if disabled or unresolvable.
    bool get_capsule_data(godot::Vector3 &out_a, float &out_radius_a,
                          godot::Vector3 &out_b, float &out_radius_b) const;

    // Dynamic property list support so the inspector can show a bone dropdown
    void _get_property_list(List<PropertyInfo> *p_list) const;
    bool _set(const StringName &p_name, const Variant &p_value);
    bool _get(const StringName &p_name, Variant &r_ret) const;

    // Exposed API
    void load_tfx_collision_asset();
    // Editor helper: list available .tfxmesh files under res://
    Array find_tfxmesh_files();
    // Editor helper: list bones from the assigned Skeleton3D (if any)
    Array find_bones();
    // Helper to register with a TressFXCharacter parent (C++-only)
    void register_to_character(class TressFXCharacter *character);

private:
    String tfx_mesh_file;
    int numCellsInXAxis = 0;
    float collisionMargin = 0.0f;
    int mesh = 0;
    String followBone;
    NodePath skeleton_node_path;

    // Use the description type from the hair node to match character registration signature
    TressFXHairNode::TressFXCollisionMeshDescription last_collision_description;

    // A3.2 subtask B: capsule authoring state.
    bool capsule_enabled = false;
    godot::Vector3 capsule_point_a = godot::Vector3(0.0f, 0.0f, 0.0f);
    godot::Vector3 capsule_point_b = godot::Vector3(0.0f, 0.1f, 0.0f);
    float capsule_radius_a = 0.1f;
    float capsule_radius_b = 0.1f;
    bool show_debug_capsule = false;

    // Runtime-only authoring aid (never created under Engine::is_editor_hint()).
    godot::MeshInstance3D* m_debug_sphere_a = nullptr;
    godot::MeshInstance3D* m_debug_sphere_b = nullptr;
    godot::Ref<godot::SphereMesh> m_debug_sphere_mesh_a;
    godot::Ref<godot::SphereMesh> m_debug_sphere_mesh_b;
    // PrimitiveMesh::set_radius/set_height fully re-tessellate the mesh and
    // re-upload it to the RenderingServer (PrimitiveMesh::request_update).
    // Cache the last-applied radius so update_debug_capsule_visual() only
    // pays that cost when the authored value actually changed, instead of
    // regenerating+re-uploading an unchanged sphere every _process() tick.
    float m_debug_sphere_applied_radius_a = -1.0f;
    float m_debug_sphere_applied_radius_b = -1.0f;
    void ensure_debug_capsule_visual();
    void clear_debug_capsule_visual();
    void update_debug_capsule_visual();
};

#endif // TRESSFX_COLLISION_NODE_H
