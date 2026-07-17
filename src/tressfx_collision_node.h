#ifndef TRESSFX_COLLISION_NODE_H
#define TRESSFX_COLLISION_NODE_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "tressfx_node.h" // reuse the collision description struct defined for hair node

using namespace godot;

class TressFXCollisionNode : public Node3D {
    GDCLASS(TressFXCollisionNode, Node3D)

protected:
    static void _bind_methods();

public:
    TressFXCollisionNode();
    ~TressFXCollisionNode();

    void _init();
    void _ready();

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

    // SDF debug/authoring follow-ups (not physics): grid padding tunable, and
    // a runtime-only voxel visualization toggle.
    void set_sdf_padding_cells(int p) { sdf_padding_cells = p; }
    int get_sdf_padding_cells() const { return sdf_padding_cells; }

    void set_show_sdf_debug(bool p) { show_sdf_debug = p; }
    bool get_show_sdf_debug() const { return show_sdf_debug; }

    void set_skeleton_node_path(const NodePath &p);
    NodePath get_skeleton_node_path() const { return skeleton_node_path; }

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
    int sdf_padding_cells = 40;
    bool show_sdf_debug = false;

    // Use the description type from the hair node to match character registration signature
    TressFXHairNode::TressFXCollisionMeshDescription last_collision_description;
};

#endif // TRESSFX_COLLISION_NODE_H
