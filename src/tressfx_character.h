#ifndef TRESSFX_CHARACTER_H
#define TRESSFX_CHARACTER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/core/class_db.hpp>
#include "tressfx_node.h"
#include "Simulation.h"
#include "HairStrands.h"
#include "SDF.h"

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

    // Gate A1 capture: fixed dt=1/60, identity bones, wind off; writes position
    // dumps after sim steps 1/30/120 to <repo>/reference_new/ for
    // tools/compare_dump.py. Off by default (normal demo behavior).
    void set_gate_capture_mode(bool enabled);
    bool get_gate_capture_mode() const;

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

    // A2.1: main-RD position texture (512x512 RGBA32F). Texel (v % 512, v / 512)
    // holds simulated hair vertex v as float4 (xyz used), updated every sim
    // tick with no CPU readback. Feeds the A2.2 ribbon vertex shader. Invalid
    // (unassigned RID) until GPU mode is active and at least one sim tick ran.
    godot::Ref<godot::Texture2DRD> get_position_texture();

private:
    std::vector<TressFXHairNode::TressFXObjectDescription> m_hairDescriptions;
    std::vector<TressFXHairNode::TressFXCollisionMeshDescription> m_collisionDescriptions;

    std::vector<std::unique_ptr<HairStrands>> m_hairStrands;
    std::vector<std::unique_ptr<CollisionMesh>> m_collisionMeshes;

    // Each adapter needs a stable EI_Scene (and thus stable Skeleton3D pointer).
    // Node-driven configuration should decide which skeleton is used.
    std::vector<std::unique_ptr<EI_Scene>> m_adapterScenes;

    std::unique_ptr<Simulation> m_pSimulation;

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

    // A1 main-RD threading. The render-thread entry points below are scheduled
    // via RenderingServer::call_on_render_thread and never run on the main
    // thread; they receive every input by value (plus raw pointers that stay
    // valid because teardown is queued behind them on the same thread).
    void _rt_initialize_gpu(int64_t sim_ptr, const godot::PackedInt64Array& hair_ptrs);
    void _rt_sim_tick(double dt, const godot::PackedFloat32Array& params,
        const godot::Array& bones_per_hair, const godot::PackedInt64Array& hair_ptrs,
        int64_t sim_ptr, int64_t gate_dump_frame);
    // Async-readback callbacks (fire on the render thread; only stash data).
    void _on_positions_async(const godot::PackedByteArray& data);
    void _on_gate_dump_async(const godot::PackedByteArray& data, int64_t frame);
    // Frees GPU-owned objects on the render thread (payload allocated by
    // teardown_gpu_runtime).
    static void _rt_destroy_gpu_payload(int64_t payload_ptr);

    // Moves the Simulation/HairStrands (and their main-RD buffers) into a heap
    // payload destroyed on the render thread. Safe to call when nothing was
    // initialized.
    void teardown_gpu_runtime();

    bool m_gate_capture_mode = false;
    std::atomic<bool> m_rt_gpu_ready{false};
    uint64_t m_sim_steps = 0;

    // Dump metadata cached on the main thread before GPU init so the gate-dump
    // callback never walks the node's containers from the render thread.
    int m_gate_vps = 0;
    int m_gate_guides = 0;
    int m_gate_stride = 0;

    // Latest async positions readback: written on the render thread,
    // consumed by _process on the main thread.
    std::mutex m_readback_mutex;
    godot::PackedByteArray m_readback_positions;
    bool m_readback_new = false;

    double m_time_seconds = 0.0;
    uint64_t m_frame_index = 0;

    godot::Ref<godot::Texture2DRD> m_gpu_guide_lines_texture;

    // A2.1: wrapper around the main-RD position texture RID (see EI_Device::
    // GetPositionTextureRID). Lazily instantiated on first get_position_texture() call.
    godot::Ref<godot::Texture2DRD> m_position_texture;
};

#endif // TRESSFX_CHARACTER_H
