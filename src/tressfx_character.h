#ifndef TRESSFX_CHARACTER_H
#define TRESSFX_CHARACTER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
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
class ShaderMaterial;
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

    // A2.4: the blue in-world GPU debug line overlay is off by default now
    // that the ribbon renderer (A2.2/A2.3) is the real visual; kept as a
    // property so it can still be re-enabled for troubleshooting.
    void set_show_gpu_debug_lines(bool enabled);
    bool get_show_gpu_debug_lines() const;

    void set_wind_velocity(const godot::Vector3& wind_velocity);
    godot::Vector3 get_wind_velocity() const;

    // Gate A1 capture: fixed dt=1/60, identity bones, wind off; writes position
    // dumps after sim steps 1/30/120 to <repo>/reference_new/ for
    // tools/compare_dump.py. Off by default (normal demo behavior).
    void set_gate_capture_mode(bool enabled);
    bool get_gate_capture_mode() const;

    // A3.1 diagnostic only: forces dt=1/60 like gate_capture_mode, but leaves
    // real bone poses and wind alone -- lets head_shake.gd's motion run under
    // fixed dt to isolate whether variable per-frame dt is what destabilizes
    // the solver under fast bone motion. Off by default; not a product feature.
    void set_debug_force_fixed_dt(bool enabled);
    bool get_debug_force_fixed_dt() const;

    // Simulation physics properties (exposed to inspector).
    void set_gravity_magnitude(float v); float get_gravity_magnitude() const;
    void set_damping(float v);           float get_damping() const;
    void set_global_stiffness(float v);  float get_global_stiffness() const;
    void set_global_range(float v);      float get_global_range() const;
    void set_local_stiffness(float v);   float get_local_stiffness() const;
    void set_clamp_position_delta(float v); float get_clamp_position_delta() const;

    // A2.2: ribbon half-width in meters (TressFX FiberRadius convention).
    void set_hair_fiber_radius(float v); float get_hair_fiber_radius() const;

    // A2.3: hair color at the root and at the tip (blended along each strand).
    void set_hair_root_color(const godot::Color& c); godot::Color get_hair_root_color() const;
    void set_hair_tip_color(const godot::Color& c);  godot::Color get_hair_tip_color() const;

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
    // Max vertex travel per sim step (meters) before the kernel clamps it.
    // 20 = AMD's default = effectively OFF at meter scale (see Simulation.h).
    float m_clamp_position_delta = 20.0f;

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

    // A2.2: ribbon-expanded hair geometry. Shares m_gpu_debug_root's transform
    // (same bone-follow parenting as the debug lines) so its mesh-local-space
    // vertices, computed straight from the A2.1 position texture with no CPU
    // involvement, land in the right place in the world.
    godot::MeshInstance3D* m_gpu_ribbon_instance = nullptr;
    godot::Ref<godot::ArrayMesh> m_gpu_ribbon_mesh;
    godot::Ref<godot::ShaderMaterial> m_gpu_ribbon_material;
    // 0.0021 is AMD's default (TressFXSettings.h), but AMD pairs it with tip
    // thinning and offset follow hairs; with those now in place the base width
    // can come down. Live-tunable in the Inspector while the game runs.
    float m_hair_fiber_radius = 0.001f;
    godot::Color m_hair_root_color = godot::Color(0.25f, 0.12f, 0.06f);
    godot::Color m_hair_tip_color  = godot::Color(0.55f, 0.35f, 0.18f);
    bool m_show_gpu_debug_lines = false;

    void build_ribbon_mesh_if_needed();

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
    bool m_debug_force_fixed_dt = false;
    std::atomic<bool> m_rt_gpu_ready{false};

    // A3 NaN watchdog: reports the FIRST non-finite or absurdly large position
    // in the per-tick async readback (and any non-finite bone matrix), then
    // goes quiet. Zero cost after it fires; kept as a tripwire for A3.2+
    // (collision) work. Proved during A3.1 that the "explosion" was never a
    // solver problem (positions stayed finite while hair "vanished").
    std::atomic<bool> m_watchdog_fired{false};
    std::atomic<bool> m_watchdog_bones_reported{false};
    std::atomic<int64_t> m_rt_tick_count{0};
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
