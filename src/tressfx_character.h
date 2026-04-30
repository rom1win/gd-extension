#ifndef TRESSFX_CHARACTER_H
#define TRESSFX_CHARACTER_H

#include <memory>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
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
class ArrayMesh;
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
    void _notification(int what);

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

    void set_debug_hair_yaw_degrees(double degrees);
    double get_debug_hair_yaw_degrees() const;

    void set_wind_velocity(const godot::Vector3& wind_velocity);
    godot::Vector3 get_wind_velocity() const;

    // Simulation physics properties (exposed to inspector).
    void set_gravity_magnitude(float v); float get_gravity_magnitude() const;
    void set_damping(float v);           float get_damping() const;
    void set_global_stiffness(float v);  float get_global_stiffness() const;
    void set_global_range(float v);      float get_global_range() const;
    void set_local_stiffness(float v);   float get_local_stiffness() const;

    // Editor helper: rebuild CPU debug line meshes (safe in editor; no RenderingDevice usage).
    void rebuild_cpu_debug_visuals();

    // GPU bring-up output: offscreen guide-lines texture rendered via RenderingDevice.
    // This is intended for debugging (TextureRect/Sprite3D/etc), not final hair rendering.
    godot::Ref<godot::Texture2DRD> get_gpu_guide_lines_texture();

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
    bool m_gpu_runtime_initialized = false;
    int m_debug_max_guide_strands = 256;

    // Extra local-space offset applied to the CPU debug visualization root.
    // Visualization-only: does not affect simulation.
    godot::Vector3 m_debug_hair_offset = godot::Vector3(0, 0, 0);

    // Additional yaw rotation applied to the CPU debug visualization (degrees).
    // Visualization-only: does not affect simulation.
    // Default: TressFX assets are typically authored +X forward; Godot is -Z forward.
    // Rotate +90° around Y to map +X -> -Z.
    double m_debug_hair_yaw_degrees = 90.0;

    // Simulation wind in world space (direction * magnitude). Default zero = no wind.
    godot::Vector3 m_wind_velocity = godot::Vector3(0, 0, 0);

    // Simulation physics tuning (RatBoy defaults).
    float m_gravity_magnitude  = 0.09f;
    float m_damping            = 0.068f;
    float m_global_stiffness   = 0.408f;
    float m_global_range       = 0.308f;
    float m_local_stiffness    = 0.908f;

    // Cached packed guide positions for optional legacy 2D overlay texture output.
    godot::PackedByteArray m_last_guide_positions_bytes;
    int m_last_vertices_per_strand = 0;
    int m_last_guide_strands = 0;
    bool m_last_guide_positions_valid = false;
    bool m_overlay_texture_requested = false;

    // Cached debug line instances so we can update meshes without recreating nodes.
    std::vector<godot::MeshInstance3D*> m_debug_line_instances;
    godot::Node* m_debug_lines_parent = nullptr;
    godot::Node3D* m_debug_root = nullptr;
    godot::BoneAttachment3D* m_debug_anchor = nullptr;

    // GPU-mode in-world debug lines (separate from CPU debug lines).
    godot::MeshInstance3D* m_gpu_debug_line_instance = nullptr;
    godot::Node3D* m_gpu_debug_root = nullptr;
    godot::BoneAttachment3D* m_gpu_debug_anchor = nullptr;
    godot::Ref<godot::ArrayMesh> m_gpu_debug_lines_mesh;

    // Cached from the collision configuration so debug lines can follow animation.
    godot::Skeleton3D* m_debug_skeleton = nullptr;
    godot::String m_debug_follow_bone;

    void refresh_debug_hair_lines();
    void clear_debug_hair_lines();
    void update_debug_hair_lines();

    void refresh_gpu_debug_hair_lines_3d();
    void clear_gpu_debug_hair_lines_3d();
    void update_gpu_debug_hair_lines_3d_transform();
    void update_gpu_debug_hair_lines_3d_mesh(const godot::PackedByteArray& pos_bytes, int vps, int guides);

    void refresh_backend_mode();
    void update_process_state();

    double m_time_seconds = 0.0;
    uint64_t m_frame_index = 0;

    godot::Ref<godot::Texture2DRD> m_gpu_guide_lines_texture;
};

#endif // TRESSFX_CHARACTER_H
