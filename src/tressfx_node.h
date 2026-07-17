#ifndef TRESSFX_NODE_H
#define TRESSFX_NODE_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

class TressFXHairNode : public Node3D {
    GDCLASS(TressFXHairNode, Node3D)

public:
    // Lightweight local descriptions that the node will populate when load is called.
    struct TressFXObjectDescription {
        String name;
        String tfx_file;
        String tfx_bone_file;
        String hair_object_name;
        int mesh_surface_index = 0;
        int num_follow_hairs = 1;
        float tip_separation = 1.0f;
        // Max random offset (meters) of each follow hair around its guide.
        // 0 = follow hairs sit exactly on their guides (invisible duplicates).
        // 0.012 is AMD's own sample value (NOTES.md issue H2).
        float follow_hair_radius = 0.012f;
    };

    struct TressFXCollisionMeshDescription {
        String name;
        String tfx_mesh_file;
        int numCellsInXAxis = 0;
        float collisionMargin = 0.0f;
        int mesh = 0;
        String followBone;
        // NodePath (string) to the Skeleton3D node in the scene. Optional — used
        // by the editor/runtime to resolve bone transforms for followBone.
        String skeleton_node_path;
        // SDF grid padding, in cells, applied to the rest-pose AABB on every
        // axis before sizing the grid (see CollisionMesh::EnsureSDFPSOCreated).
        // 40 matches AMD's own derivation (0.8 * numCellsInXAxis) at the
        // scene's default numCellsInXAxis=50, so this default is a no-op.
        int sdf_padding_cells = 40;
    };

protected:
    static void _bind_methods();

public:
    TressFXHairNode();
    ~TressFXHairNode();

    void _init();
    void _ready();

    // Inspector properties (simple accessors)
    void set_tfx_file(const String &p) { tfx_file = p; }
    String get_tfx_file() const { return tfx_file; }

    void set_tfx_bone_file(const String &p) { tfx_bone_file = p; }
    String get_tfx_bone_file() const { return tfx_bone_file; }

    void set_hair_object_name(const String &p) { hair_object_name = p; }
    String get_hair_object_name() const { return hair_object_name; }

    void set_mesh_surface_index(int p) { mesh_surface_index = p; }
    int get_mesh_surface_index() const { return mesh_surface_index; }

    void set_num_follow_hairs(int p) { num_follow_hairs = p; }
    int get_num_follow_hairs() const { return num_follow_hairs; }

    void set_tip_separation(float p) { tip_separation = p; }
    float get_tip_separation() const { return tip_separation; }

    void set_follow_hair_radius(float p) { follow_hair_radius = p; }
    float get_follow_hair_radius() const { return follow_hair_radius; }

    // Public API exposed to scripts
    void load_tfx_asset();
    void bind_to_godot_mesh(const NodePath &mesh_node_path);
    // Find .tfx/.tfxmesh/.tfxbone files under res:// (editor helper)
    Array find_tfx_files();
    // Helper to register with a TressFXCharacter parent (C++-only)
    void register_to_character(class TressFXCharacter *character);

private:
    // stored inspector values
    String tfx_file;
    String tfx_bone_file;
    String hair_object_name;
    int mesh_surface_index = 0;
    int num_follow_hairs = 1;
    float tip_separation = 1.0f;
    float follow_hair_radius = 0.012f;

    // node path to target mesh (set via inspector or bind call)
    NodePath target_mesh_path;

    // last-created descriptions
    TressFXObjectDescription last_object_description;
    TressFXCollisionMeshDescription last_collision_description;
};

#endif // TRESSFX_NODE_H
