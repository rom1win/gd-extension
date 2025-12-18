#ifndef TRESSFX_CHARACTER_H
#define TRESSFX_CHARACTER_H

#include <memory>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/core/class_db.hpp>
#include "tressfx_node.h"
#include "Simulation.h"
#include "HairStrands.h"
#include "SDF.h"
#include "TressFX/TressFXPPLL.h"
#include "TressFX/TressFXShortCut.h"

using namespace godot;

class EI_Scene;

namespace godot {
class Skeleton3D;
class MeshInstance3D;
class Node;
class Node3D;
class BoneAttachment3D;
}

class TressFXCharacter : public Node3D {
    GDCLASS(TressFXCharacter, Node3D)

public:
    TressFXCharacter();
    ~TressFXCharacter();

    static void _bind_methods();

    void _init();
    void _ready();
    void _process(double delta);

    // Registration API used by child hair/collision nodes
    void register_hair_description(const TressFXHairNode::TressFXObjectDescription &desc);
    void register_collision_description(const TressFXHairNode::TressFXCollisionMeshDescription &desc);

    // Trigger creation of the TressFX runtime objects (stub for now)
    void load_all_assets();

    void set_debug_draw_hair_lines(bool enabled);
    bool get_debug_draw_hair_lines() const;

    void set_debug_max_guide_strands(int max_strands);
    int get_debug_max_guide_strands() const;

    void set_debug_hair_offset(const godot::Vector3& offset);
    godot::Vector3 get_debug_hair_offset() const;

private:
    std::vector<TressFXHairNode::TressFXObjectDescription> m_hairDescriptions;
    std::vector<TressFXHairNode::TressFXCollisionMeshDescription> m_collisionDescriptions;

    std::vector<std::unique_ptr<HairStrands>> m_hairStrands;
    std::vector<std::unique_ptr<CollisionMesh>> m_collisionMeshes;

    // Each adapter needs a stable EI_Scene (and thus stable Skeleton3D pointer).
    // Node-driven configuration should decide which skeleton is used.
    std::vector<std::unique_ptr<EI_Scene>> m_adapterScenes;

    std::unique_ptr<Simulation> m_pSimulation;
    std::unique_ptr<TressFXPPLL> m_pPPLL;
    std::unique_ptr<TressFXShortCut> m_pShortCut;

    // Debug toggle:
    // - enabled: show CPU guide-line visualization (rigid bone-follow, no skinning)
    // - disabled: use the GPU path (simulation/rendering bring-up)
    bool m_debug_draw_hair_lines = false;
    bool m_gpu_mode_active = false;
    int m_debug_max_guide_strands = 256;

    // Extra local-space offset applied to the debug root (after the facing adjustment).
    // Useful to correct a constant rest-pose misalignment between the exported hair asset
    // coordinate system and the Godot character/skeleton setup.
    godot::Vector3 m_debug_hair_offset = godot::Vector3(0, 0, 0);

    // Cached debug line instances so we can update meshes without recreating nodes.
    std::vector<godot::MeshInstance3D*> m_debug_line_instances;
    godot::Node* m_debug_lines_parent = nullptr;
    godot::Node3D* m_debug_root = nullptr;
    godot::BoneAttachment3D* m_debug_anchor = nullptr;

    // Cached from the collision configuration so debug lines can follow animation.
    godot::Skeleton3D* m_debug_skeleton = nullptr;
    godot::String m_debug_follow_bone;

    void refresh_debug_hair_lines();
    void clear_debug_hair_lines();
    void update_debug_hair_lines();

    void refresh_backend_mode();
    void update_process_state();

    double m_time_seconds = 0.0;
    uint64_t m_frame_index = 0;
};

#endif // TRESSFX_CHARACTER_H
