#include "tressfx_character.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/math.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include "tressfx_collision_node.h"
#include "EngineInterface.h"
#include "Simulation.h"
#include "HairStrands.h"
#include "SDF.h"
#include <cmath>
#include "GodotScene.h"

using namespace godot;

// A1: GPU-owned state (kernel PSOs, main-RD buffers inside the hair objects)
// must be destroyed on the render thread. teardown_gpu_runtime() moves the
// owning pointers out of the node into this heap payload and schedules
// _rt_destroy_gpu_payload; render-thread callables run in FIFO order, so any
// still-pending sim tick executes before the payload is freed.
struct TressFXGpuTeardownPayload {
    std::unique_ptr<Simulation> sim;
    std::vector<std::unique_ptr<HairStrands>> hair;
    std::vector<std::unique_ptr<CollisionMesh>> coll;
    std::vector<std::unique_ptr<EI_Scene>> scenes;
};

// Identity skinning matrices for the whole sim UBO bone array (gate capture
// runs with a frozen skeleton, matching the reference dumps).
static PackedByteArray make_identity_bone_matrices() {
    constexpr int kBoneCount = 128;   // g_BoneSkinningMatrix[128] in the kernels
    constexpr int kFloatsPerMat = 16; // float4x4
    PackedByteArray out;
    out.resize(kBoneCount * kFloatsPerMat * 4); // resize() zero-fills
    uint8_t* w = out.ptrw();
    const float one = 1.0f;
    for (int m = 0; m < kBoneCount; ++m) {
        for (int d = 0; d < 4; ++d) {
            memcpy(w + ((size_t)m * kFloatsPerMat + (size_t)d * 5) * 4, &one, 4);
        }
    }
    return out;
}

void TressFXCharacter::_bind_methods() {
    // Only expose load_all_assets to scripting for now. The register_* methods are
    // C++-only and accept POD structs that are not bindable to Variant automatically.
    ClassDB::bind_method(D_METHOD("load_all_assets"), &TressFXCharacter::load_all_assets);

    ClassDB::bind_method(D_METHOD("set_debug_draw_hair_lines", "enabled"), &TressFXCharacter::set_debug_draw_hair_lines);
    ClassDB::bind_method(D_METHOD("get_debug_draw_hair_lines"), &TressFXCharacter::get_debug_draw_hair_lines);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_draw_hair_lines"), "set_debug_draw_hair_lines", "get_debug_draw_hair_lines");

    ClassDB::bind_method(D_METHOD("set_debug_max_guide_strands", "max_strands"), &TressFXCharacter::set_debug_max_guide_strands);
    ClassDB::bind_method(D_METHOD("get_debug_max_guide_strands"), &TressFXCharacter::get_debug_max_guide_strands);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_max_guide_strands", PROPERTY_HINT_RANGE, "1,256,1"), "set_debug_max_guide_strands", "get_debug_max_guide_strands");

    ClassDB::bind_method(D_METHOD("set_debug_hair_offset", "offset"), &TressFXCharacter::set_debug_hair_offset);
    ClassDB::bind_method(D_METHOD("get_debug_hair_offset"), &TressFXCharacter::get_debug_hair_offset);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "debug_hair_offset"), "set_debug_hair_offset", "get_debug_hair_offset");

    ClassDB::bind_method(D_METHOD("set_debug_hair_yaw_degrees", "degrees"), &TressFXCharacter::set_debug_hair_yaw_degrees);
    ClassDB::bind_method(D_METHOD("get_debug_hair_yaw_degrees"), &TressFXCharacter::get_debug_hair_yaw_degrees);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "debug_hair_yaw_degrees", PROPERTY_HINT_RANGE, "-180,180,0.1"), "set_debug_hair_yaw_degrees", "get_debug_hair_yaw_degrees");

    ClassDB::bind_method(D_METHOD("set_show_gpu_debug_lines", "enabled"), &TressFXCharacter::set_show_gpu_debug_lines);
    ClassDB::bind_method(D_METHOD("get_show_gpu_debug_lines"), &TressFXCharacter::get_show_gpu_debug_lines);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "show_gpu_debug_lines"), "set_show_gpu_debug_lines", "get_show_gpu_debug_lines");

    ClassDB::bind_method(D_METHOD("set_wind_velocity", "wind_velocity"), &TressFXCharacter::set_wind_velocity);
    ClassDB::bind_method(D_METHOD("get_wind_velocity"), &TressFXCharacter::get_wind_velocity);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind_velocity"), "set_wind_velocity", "get_wind_velocity");

    ClassDB::bind_method(D_METHOD("set_gate_capture_mode", "enabled"), &TressFXCharacter::set_gate_capture_mode);
    ClassDB::bind_method(D_METHOD("get_gate_capture_mode"), &TressFXCharacter::get_gate_capture_mode);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "gate_capture_mode"), "set_gate_capture_mode", "get_gate_capture_mode");

    ClassDB::bind_method(D_METHOD("set_debug_force_fixed_dt", "enabled"), &TressFXCharacter::set_debug_force_fixed_dt);
    ClassDB::bind_method(D_METHOD("get_debug_force_fixed_dt"), &TressFXCharacter::get_debug_force_fixed_dt);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "debug_force_fixed_dt"), "set_debug_force_fixed_dt", "get_debug_force_fixed_dt");

    ClassDB::bind_method(D_METHOD("set_gravity_magnitude", "v"), &TressFXCharacter::set_gravity_magnitude);
    ClassDB::bind_method(D_METHOD("get_gravity_magnitude"), &TressFXCharacter::get_gravity_magnitude);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "gravity_magnitude", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_gravity_magnitude", "get_gravity_magnitude");

    ClassDB::bind_method(D_METHOD("set_damping", "v"), &TressFXCharacter::set_damping);
    ClassDB::bind_method(D_METHOD("get_damping"), &TressFXCharacter::get_damping);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_damping", "get_damping");

    ClassDB::bind_method(D_METHOD("set_global_stiffness", "v"), &TressFXCharacter::set_global_stiffness);
    ClassDB::bind_method(D_METHOD("get_global_stiffness"), &TressFXCharacter::get_global_stiffness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "global_stiffness", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_global_stiffness", "get_global_stiffness");

    ClassDB::bind_method(D_METHOD("set_global_range", "v"), &TressFXCharacter::set_global_range);
    ClassDB::bind_method(D_METHOD("get_global_range"), &TressFXCharacter::get_global_range);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "global_range", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_global_range", "get_global_range");

    ClassDB::bind_method(D_METHOD("set_local_stiffness", "v"), &TressFXCharacter::set_local_stiffness);
    ClassDB::bind_method(D_METHOD("get_local_stiffness"), &TressFXCharacter::get_local_stiffness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "local_stiffness", PROPERTY_HINT_RANGE, "0.0,1.0,0.001"), "set_local_stiffness", "get_local_stiffness");

    ClassDB::bind_method(D_METHOD("set_clamp_position_delta", "v"), &TressFXCharacter::set_clamp_position_delta);
    ClassDB::bind_method(D_METHOD("get_clamp_position_delta"), &TressFXCharacter::get_clamp_position_delta);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clamp_position_delta", PROPERTY_HINT_RANGE, "0.001,20.0,0.001"), "set_clamp_position_delta", "get_clamp_position_delta");

    ClassDB::bind_method(D_METHOD("set_collision_enabled", "v"), &TressFXCharacter::set_collision_enabled);
    ClassDB::bind_method(D_METHOD("get_collision_enabled"), &TressFXCharacter::get_collision_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "get_collision_enabled");

    ClassDB::bind_method(D_METHOD("set_hair_fiber_radius", "v"), &TressFXCharacter::set_hair_fiber_radius);
    ClassDB::bind_method(D_METHOD("get_hair_fiber_radius"), &TressFXCharacter::get_hair_fiber_radius);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hair_fiber_radius", PROPERTY_HINT_RANGE, "0.0001,0.02,0.0001"), "set_hair_fiber_radius", "get_hair_fiber_radius");

    ClassDB::bind_method(D_METHOD("set_hair_root_color", "c"), &TressFXCharacter::set_hair_root_color);
    ClassDB::bind_method(D_METHOD("get_hair_root_color"), &TressFXCharacter::get_hair_root_color);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "hair_root_color"), "set_hair_root_color", "get_hair_root_color");

    ClassDB::bind_method(D_METHOD("set_hair_tip_color", "c"), &TressFXCharacter::set_hair_tip_color);
    ClassDB::bind_method(D_METHOD("get_hair_tip_color"), &TressFXCharacter::get_hair_tip_color);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "hair_tip_color"), "set_hair_tip_color", "get_hair_tip_color");

    ClassDB::bind_method(D_METHOD("rebuild_cpu_debug_visuals"), &TressFXCharacter::rebuild_cpu_debug_visuals);

    // Keep as an explicit method for optional overlay viewers; do not expose as an inspector property.
    ClassDB::bind_method(D_METHOD("get_gpu_guide_lines_texture"), &TressFXCharacter::get_gpu_guide_lines_texture);

    // A2.1: explicit method (not an inspector property) so a ShaderMaterial/
    // debug viewer can fetch it via get_position_texture().
    ClassDB::bind_method(D_METHOD("get_position_texture"), &TressFXCharacter::get_position_texture);
}

TressFXCharacter::TressFXCharacter() {}

TressFXCharacter::~TressFXCharacter() {}

void TressFXCharacter::_init() {}

void TressFXCharacter::_ready() {
    const bool in_editor = (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint());

    // Register any existing child hair/collision nodes in case they were created
    // before this character (handles creation order in the editor).
    int cnt = get_child_count();
    for (int i = 0; i < cnt; ++i) {
        Node *c = get_child(i);
        if (!c) continue;
        TressFXHairNode *hn = Object::cast_to<TressFXHairNode>(c);
        if (hn) {
            hn->register_to_character(this);
            continue;
        }
        TressFXCollisionNode *cn = Object::cast_to<TressFXCollisionNode>(c);
        if (cn) {
            cn->register_to_character(this);
            continue;
        }
    }

    // Now that all children have registered their descriptions, 
    // we can initialize the TressFX engine and load the assets.
    // Use call_deferred to ensure ALL _ready() calls in the scene tree have completed
    // before loading, preventing race conditions where children register AFTER the parent's
    // _ready() has already called load_all_assets().
    if (in_editor) {
        // Editor: allow CPU debug visualization as an alignment tool.
        // Do NOT initialize GPU/RenderingDevice here.
        if (m_debug_draw_hair_lines) {
            call_deferred("load_all_assets");
        }
        update_process_state();
        return;
    }

    call_deferred("load_all_assets");
    update_process_state();
}

void TressFXCharacter::_process(double delta) {
    const bool in_editor = (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint());
    if (in_editor) {
        // Editor: only keep CPU debug alignment updated.
        if (m_debug_draw_hair_lines) {
            update_debug_hair_lines();
        }
        return;
    }

    m_frame_index++;
    m_time_seconds += delta;

    if (m_debug_draw_hair_lines) {
        update_debug_hair_lines();
    }

    if (!m_gpu_mode_active) {
        return;
    }

    update_gpu_debug_hair_lines_3d_transform();

    // A1: late init. The Simulation host object is created here on the main
    // thread, but ALL its GPU work (kernel PSO compile, buffer creation) runs
    // on the render thread. No RenderingDevice call happens on the main thread.
    if (!m_gpu_runtime_initialized) {
        if (!GetDevice() || m_hairStrands.empty()) {
            return;
        }
        m_pSimulation = std::make_unique<Simulation>();

        // Cache dump metadata now: the gate-dump callback runs on the render
        // thread and must not walk the node's containers.
        m_gate_vps    = m_hairStrands[0]->GetVertsPerStrand();
        m_gate_guides = m_hairStrands[0]->GetGuideStrandCount();
        m_gate_stride = m_hairStrands[0]->GetFollowPerGuide() + 1;

        PackedInt64Array hair_ptrs;
        for (auto& h : m_hairStrands) {
            hair_ptrs.push_back((int64_t)(intptr_t)h.get());
        }

        // Arm state BEFORE scheduling: with single-threaded rendering (Godot's
        // default), call_on_render_thread executes the callable INLINE, so
        // _rt_initialize_gpu sets m_rt_gpu_ready before this call returns —
        // writing these flags afterwards would clobber it.
        m_gpu_runtime_initialized = true;
        m_rt_gpu_ready.store(false);
        m_sim_steps = 0;

        RenderingServer::get_singleton()->call_on_render_thread(
            callable_mp(this, &TressFXCharacter::_rt_initialize_gpu)
                .bind((int64_t)(intptr_t)m_pSimulation.get(), hair_ptrs));
        if (m_gate_capture_mode) {
            UtilityFunctions::print("TressFXCharacter: GATE A1 capture mode — fixed dt=1/60, identity bones, wind off; dumps at sim steps 1/30/120");
        }
    }

    // Schedule one simulation step on the render thread. Every input is
    // snapshotted by value here; the render thread never touches scene nodes.
    if (m_pSimulation && m_rt_gpu_ready.load()) {
        Array bones_per_hair;
        for (auto& h : m_hairStrands) {
            if (m_gate_capture_mode) {
                bones_per_hair.append(make_identity_bone_matrices());
            } else {
                bones_per_hair.append(h ? h->SnapshotBoneMatrices() : PackedByteArray());
            }
        }

        const Vector3 wind = m_gate_capture_mode ? Vector3() : m_wind_velocity;
        PackedFloat32Array params;
        params.resize(9);
        params[0] = (float)wind.x;
        params[1] = (float)wind.y;
        params[2] = (float)wind.z;
        params[3] = m_gravity_magnitude;
        params[4] = m_damping;
        params[5] = m_global_stiffness;
        params[6] = m_global_range;
        params[7] = m_local_stiffness;
        params[8] = m_clamp_position_delta;

        // A3.2 subtask B: resolve each registered capsule's current pose on
        // THIS (main) thread -- Skeleton3D::get_bone_global_pose must not be
        // called from the render thread. Packed as 8 floats/capsule
        // (xyz_a, radius_a, xyz_b, radius_b); count 0 whenever collision is
        // off, which keeps the regression gate byte-for-byte unaffected.
        PackedFloat32Array capsule_data;
        if (m_collision_enabled && !m_gate_capture_mode) {
            int count = 0;
            for (TressFXCollisionNode* node : m_capsuleNodes) {
                if (!node) {
                    continue;
                }
                Vector3 a, b;
                float ra = 0.0f, rb = 0.0f;
                if (!node->get_capsule_data(a, ra, b, rb)) {
                    continue;
                }
                if (count >= 8) {
                    if (!m_capsule_overflow_warned) {
                        m_capsule_overflow_warned = true;
                        UtilityFunctions::push_warning("TressFXCharacter: more than 8 enabled collision capsules; extras ignored (TRESSFX_MAX_NUM_COLLISION_CAPSULES=8).");
                    }
                    break;
                }
                capsule_data.push_back((float)a.x);
                capsule_data.push_back((float)a.y);
                capsule_data.push_back((float)a.z);
                capsule_data.push_back(ra);
                capsule_data.push_back((float)b.x);
                capsule_data.push_back((float)b.y);
                capsule_data.push_back((float)b.z);
                capsule_data.push_back(rb);
                count++;
            }
        }

        const double dt = (m_gate_capture_mode || m_debug_force_fixed_dt) ? (1.0 / 60.0) : delta;

        m_sim_steps++;
        int64_t gate_dump_frame = 0;
        if (m_gate_capture_mode && (m_sim_steps == 1 || m_sim_steps == 30 || m_sim_steps == 120)) {
            gate_dump_frame = (int64_t)m_sim_steps;
        }

        PackedInt64Array hair_ptrs;
        for (auto& h : m_hairStrands) {
            hair_ptrs.push_back((int64_t)(intptr_t)h.get());
        }

        RenderingServer::get_singleton()->call_on_render_thread(
            callable_mp(this, &TressFXCharacter::_rt_sim_tick)
                .bind(dt, params, bones_per_hair, hair_ptrs,
                      (int64_t)(intptr_t)m_pSimulation.get(), gate_dump_frame, capsule_data));
    }

    // Consume the latest async readback. Guide positions arrive one or two
    // frames late; that is the price of never stalling the GPU.
    PackedByteArray latest;
    {
        std::lock_guard<std::mutex> lock(m_readback_mutex);
        if (m_readback_new) {
            latest = m_readback_positions;
            m_readback_new = false;
        }
    }

    if (!m_hairStrands.empty()) {
        godot::PackedByteArray pos_bytes;
        int vps = 0;
        int guides = 0;
        bool ok = false;
        if (latest.size() > 0) {
            ok = m_hairStrands[0]->ExtractGuidePositionsVec4FromBytes(latest, m_debug_max_guide_strands, pos_bytes, vps, guides);
        } else if (!m_last_guide_positions_valid) {
            // Nothing read back yet (first frames): show rest positions.
            ok = m_hairStrands[0]->PackGuidePositionsVec4(pos_bytes, vps, guides);
        }
        if (ok) {
            m_last_guide_positions_bytes = pos_bytes;
            m_last_vertices_per_strand = vps;
            m_last_guide_strands = guides;
            m_last_guide_positions_valid = true;

            refresh_gpu_debug_hair_lines_3d();
            update_gpu_debug_hair_lines_3d_mesh(pos_bytes, vps, guides);
        }
    }

    // A2.2: keep the ribbon material's texture parameter pointed at the current
    // position texture. Cheap (state read only, no RenderingDevice work) --
    // needed because the texture RID may not exist yet the first time the
    // material is created (before the first sim tick has run).
    if (m_gpu_ribbon_material.is_valid()) {
        Ref<Texture2DRD> tex = get_position_texture();
        if (tex.is_valid()) {
            m_gpu_ribbon_material->set_shader_parameter("position_tex", tex);
        }
    }

    // Optional legacy overlay render: only execute when requested by a UI viewer.
    if (m_overlay_texture_requested && m_last_guide_positions_valid) {
        if (EI_Device* device = GetDevice()) {
            device->SetGuideLinesSource(m_last_guide_positions_bytes, m_last_vertices_per_strand, m_last_guide_strands);
            device->RunMainRDGuideLinesOnce();
        }
    }
    m_overlay_texture_requested = false;
}

void TressFXCharacter::_rt_initialize_gpu(int64_t sim_ptr, const PackedInt64Array& hair_ptrs) {
    // RENDER thread. Compile the kernel PSOs and create the GPU hair objects on
    // the main RenderingDevice.
    Simulation* sim = reinterpret_cast<Simulation*>((intptr_t)sim_ptr);
    if (!sim) {
        return;
    }
    sim->Initialize();
    for (int64_t i = 0; i < hair_ptrs.size(); ++i) {
        HairStrands* h = reinterpret_cast<HairStrands*>((intptr_t)hair_ptrs[i]);
        if (h) {
            h->EnsureTressFXObjectCreated();
        }
    }
    m_rt_gpu_ready.store(true);
}

void TressFXCharacter::_rt_sim_tick(double dt, const PackedFloat32Array& params,
        const Array& bones_per_hair, const PackedInt64Array& hair_ptrs,
        int64_t sim_ptr, int64_t gate_dump_frame,
        const PackedFloat32Array& capsule_data) {
    // RENDER thread. Inputs arrive by value; the sim/hair pointers stay valid
    // because teardown_gpu_runtime queues destruction behind this call (FIFO).
    Simulation* sim = reinterpret_cast<Simulation*>((intptr_t)sim_ptr);
    if (!sim) {
        return;
    }

    SimulationContext ctx;
    ctx.hairStrands.reserve((size_t)hair_ptrs.size());
    for (int64_t i = 0; i < hair_ptrs.size(); ++i) {
        ctx.hairStrands.push_back(reinterpret_cast<HairStrands*>((intptr_t)hair_ptrs[i]));
        if (i < bones_per_hair.size()) {
            ctx.bone_matrices.push_back((PackedByteArray)bones_per_hair[i]);
        }
    }

    if (params.size() >= 8) {
        ctx.wind_velocity             = Vector3(params[0], params[1], params[2]);
        ctx.gravityMagnitude          = params[3];
        ctx.damping                   = params[4];
        ctx.globalConstraintStiffness = params[5];
        ctx.globalConstraintsRange    = params[6];
        ctx.localConstraintStiffness  = params[7];
    }
    if (params.size() >= 9) {
        ctx.clampPositionDelta        = params[8];
    }

    // A3.2 subtask B: 8 floats per capsule (xyz_a, radius_a, xyz_b, radius_b),
    // already resolved to SKELETON MODEL SPACE on the main thread. Empty
    // whenever collision_enabled is false (see _process).
    const int64_t numCapsuleFloats = capsule_data.size() - (capsule_data.size() % 8);
    for (int64_t i = 0; i + 8 <= numCapsuleFloats; i += 8) {
        CollisionCapsuleData c;
        c.center_a = Vector3(capsule_data[i + 0], capsule_data[i + 1], capsule_data[i + 2]);
        c.radius_a = capsule_data[i + 3];
        c.center_b = Vector3(capsule_data[i + 4], capsule_data[i + 5], capsule_data[i + 6]);
        c.radius_b = capsule_data[i + 7];
        ctx.collisionCapsules.push_back(c);
    }

    // A3.1 watchdog, input side: are the bone matrices we are about to feed
    // the kernels already poisoned? (Tiny scan: <= bones x 16 floats per hair.)
    const int64_t tick = ++m_rt_tick_count;
    if (!m_watchdog_bones_reported.load()) {
        for (size_t h = 0; h < ctx.bone_matrices.size() && !m_watchdog_bones_reported.load(); ++h) {
            const PackedByteArray& b = ctx.bone_matrices[h];
            const float* f = reinterpret_cast<const float*>(b.ptr());
            const int64_t n = (int64_t)b.size() / 4;
            for (int64_t i = 0; i < n; ++i) {
                if (!std::isfinite(f[i])) {
                    m_watchdog_bones_reported.store(true);
                    UtilityFunctions::print("TressFX WATCHDOG: NON-FINITE BONE MATRIX at sim tick ", tick,
                        " hair=", (int64_t)h, " bone=", i / 16, " element=", i % 16, " value=", f[i]);
                    break;
                }
            }
        }
    }

    sim->StartSimulation(dt, ctx, /*bUpdateCollMesh=*/false, /*bSDFCollisionResponse=*/false, /*bAsync=*/false);

    // Queue async readbacks AFTER the dispatches so the data reflects this step.
    if (ctx.hairStrands.empty() || !ctx.hairStrands[0]) {
        return;
    }
    const RID rid = ctx.hairStrands[0]->GetPositionsBufferRID();
    RenderingServer* rs = RenderingServer::get_singleton();
    RenderingDevice* rd = rs ? rs->get_rendering_device() : nullptr;
    static bool s_tick_logged = false;
    if (!s_tick_logged) {
        s_tick_logged = true;
        UtilityFunctions::print("TressFXCharacter: first sim tick on render thread (rd=",
            rd ? "ok" : "null", " positions_rid_valid=", rid.is_valid() ? "yes" : "no", ")");
    }
    if (!rd || !rid.is_valid()) {
        return;
    }

    // A2.1: GPU-to-GPU feed for the ribbon renderer. Copies every simulated
    // vertex (guides + follow hairs) into the position texture; no CPU
    // readback on this path.
    if (EI_Device* device = GetDevice()) {
        const int vertex_count = ctx.hairStrands[0]->GetTotalStrandCount() * ctx.hairStrands[0]->GetVertsPerStrand();
        device->DispatchPositionTextureCopy(rid, vertex_count);
    }

    const Error err = rd->buffer_get_data_async(rid, callable_mp(this, &TressFXCharacter::_on_positions_async));
    static bool s_readback_logged = false;
    if (!s_readback_logged) {
        s_readback_logged = true;
        UtilityFunctions::print("TressFXCharacter: buffer_get_data_async err=", (int64_t)err);
    }
    if (gate_dump_frame > 0) {
        rd->buffer_get_data_async(rid,
            callable_mp(this, &TressFXCharacter::_on_gate_dump_async).bind(gate_dump_frame));
    }
}

void TressFXCharacter::_on_positions_async(const PackedByteArray& data) {
    // A3.1 watchdog, output side: find the FIRST vertex the sim corrupted.
    // Full scan per tick until it fires once, then permanently quiet.
    if (!m_watchdog_fired.load() && data.size() >= 16 && m_gate_vps > 0) {
        const float* f = reinterpret_cast<const float*>(data.ptr());
        const int64_t n = (int64_t)data.size() / 4;
        int64_t bad = -1;
        float max_abs = 0.0f;
        int64_t max_idx = 0;
        for (int64_t i = 0; i < n; ++i) {
            if ((i & 3) == 3) continue; // skip w components (inverse mass, not a position)
            const float v = f[i];
            if (!std::isfinite(v)) { bad = i; break; }
            const float a = std::abs(v);
            if (a > max_abs) { max_abs = a; max_idx = i; }
        }
        // No NaN/inf but a vertex kilometers away is the same failure teleporting.
        const bool teleport = (bad < 0 && max_abs > 1000.0f);
        if (bad >= 0 || teleport) {
            m_watchdog_fired.store(true);
            const int64_t fidx = (bad >= 0) ? bad : max_idx;
            const int64_t vertex = fidx / 4;
            const int64_t strand = vertex / m_gate_vps;
            const int64_t local = vertex % m_gate_vps;
            const int64_t slot = (m_gate_stride > 0) ? (strand % m_gate_stride) : 0;
            UtilityFunctions::print("TressFX WATCHDOG: first bad position at sim tick ~", m_rt_tick_count.load(),
                (bad >= 0) ? " (NON-FINITE)" : " (TELEPORT)",
                ": vertex=", vertex,
                " strand=", strand, (slot == 0) ? " [GUIDE]" : " [follow]",
                " local_vertex=", local, "/", (int64_t)m_gate_vps,
                " xyz=(", f[vertex * 4 + 0], ", ", f[vertex * 4 + 1], ", ", f[vertex * 4 + 2], ")");
        }
    }

    // Render-thread callback: just stash; _process consumes on the main thread.
    std::lock_guard<std::mutex> lock(m_readback_mutex);
    m_readback_positions = data;
    m_readback_new = true;
}

void TressFXCharacter::_on_gate_dump_async(const PackedByteArray& data, int64_t frame) {
    // Render-thread callback: file I/O only, no scene access. Same format and
    // filenames as the GDScript reference capture so tools/compare_dump.py
    // compares them directly.
    const String dir = ProjectSettings::get_singleton()->globalize_path("res://").path_join("../reference_new");
    DirAccess::make_dir_recursive_absolute(dir);
    const String path = dir.path_join(String("gdscript_ref_frame_") + String::num_int64(frame).pad_zeros(3) + String(".bin"));
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
    if (f.is_null()) {
        UtilityFunctions::push_error(String("TressFXCharacter: cannot write gate dump ") + path);
        return;
    }
    f->store_32((uint32_t)m_gate_vps);
    f->store_32((uint32_t)m_gate_guides);
    f->store_32((uint32_t)m_gate_stride);
    f->store_32((uint32_t)frame);
    f->store_buffer(data);
    f->close();
    UtilityFunctions::print("TressFXCharacter: GATE A1 dump written: ", path);
}

void TressFXCharacter::_rt_destroy_gpu_payload(int64_t payload_ptr) {
    // RENDER thread. Frees the main-RD GPU resources owned by the hair objects.
    delete reinterpret_cast<TressFXGpuTeardownPayload*>((intptr_t)payload_ptr);
}

void TressFXCharacter::teardown_gpu_runtime() {
    if (!m_gpu_runtime_initialized) {
        // GPU never touched: plain main-thread destruction is safe.
        m_pSimulation.reset();
        return;
    }

    TressFXGpuTeardownPayload* payload = new TressFXGpuTeardownPayload();
    payload->sim    = std::move(m_pSimulation);
    payload->hair   = std::move(m_hairStrands);
    payload->coll   = std::move(m_collisionMeshes);
    payload->scenes = std::move(m_adapterScenes);
    m_hairStrands.clear();
    m_collisionMeshes.clear();
    m_adapterScenes.clear();

    m_gpu_runtime_initialized = false;
    m_rt_gpu_ready.store(false);
    m_sim_steps = 0;
    m_last_guide_positions_valid = false;

    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs) {
        rs->call_on_render_thread(
            callable_mp_static(&TressFXCharacter::_rt_destroy_gpu_payload)
                .bind((int64_t)(intptr_t)payload));
    } else {
        // Engine already gone (late shutdown): nothing else will touch the GPU.
        delete payload;
    }
}

void TressFXCharacter::register_hair_description(const TressFXHairNode::TressFXObjectDescription &desc) {
    // Dedupe by tfx_file to avoid duplicate registrations caused by both child
    // self-registration and parent discovery.
    for (const auto &existing : m_hairDescriptions) {
        if (existing.tfx_file == desc.tfx_file) {
            UtilityFunctions::print(String("TressFXCharacter: hair already registered (skipping): ") + desc.tfx_file);
            return;
        }
    }
    m_hairDescriptions.push_back(desc);
    UtilityFunctions::print(String("TressFXCharacter: registered hair: ") + desc.tfx_file);
}

void TressFXCharacter::register_collision_description(const TressFXHairNode::TressFXCollisionMeshDescription &desc) {
    // Dedupe by tfx_mesh_file to avoid duplicate registrations.
    for (const auto &existing : m_collisionDescriptions) {
        if (existing.tfx_mesh_file == desc.tfx_mesh_file) {
            UtilityFunctions::print(String("TressFXCharacter: collision already registered (skipping): ") + desc.tfx_mesh_file);
            return;
        }
    }
    m_collisionDescriptions.push_back(desc);
    UtilityFunctions::print(
        String("TressFXCharacter: registered collision: ") + desc.tfx_mesh_file +
        String(" followBone='") + desc.followBone +
        String("' skeleton_node_path='") + desc.skeleton_node_path +
        String("'"));
}

void TressFXCharacter::register_capsule_node(TressFXCollisionNode *node) {
    if (!node) {
        return;
    }
    // Both TressFXCollisionNode::_ready() and the discovery loop in this
    // node's own _ready() can register the same node; dedupe by identity.
    for (TressFXCollisionNode* existing : m_capsuleNodes) {
        if (existing == node) {
            return;
        }
    }
    m_capsuleNodes.push_back(node);
}

void TressFXCharacter::load_all_assets() {
    // For now just print a summary of the collected descriptions. In next steps this
    // will call into the TressFX engine interface to create HairStrands/CollisionMeshes.
    UtilityFunctions::print(String("TressFXCharacter: load_all_assets called"));
    UtilityFunctions::print(String("  Hair count: ") + String::num_int64((int)m_hairDescriptions.size()));
    for (int i = 0; i < (int)m_hairDescriptions.size(); ++i) {
        const auto &d = m_hairDescriptions[i];
        UtilityFunctions::print(String("   [") + String::num_int64(i) + String("] ") + d.tfx_file + String(" -> hair object: ") + d.hair_object_name);
    }

    UtilityFunctions::print(String("  Collision count: ") + String::num_int64((int)m_collisionDescriptions.size()));
    for (int i = 0; i < (int)m_collisionDescriptions.size(); ++i) {
        const auto &d = m_collisionDescriptions[i];
        UtilityFunctions::print(String("   [") + String::num_int64(i) + String("] ") + d.tfx_mesh_file + String(" -> followBone: ") + d.followBone);
    }

    // Node-driven skeleton selection:
    // - Collision nodes can provide `skeleton_node_path`.
    // - We treat the first valid one as the default skeleton for hair.
    Skeleton3D* default_skeleton = nullptr;
    String default_skeleton_path;
    for (const auto& d : m_collisionDescriptions) {
        if (d.skeleton_node_path.is_empty()) {
            continue;
        }
        Node* node = get_node_or_null(NodePath(d.skeleton_node_path));
        Skeleton3D* sk = Object::cast_to<Skeleton3D>(node);
        if (sk) {
            default_skeleton = sk;
            default_skeleton_path = d.skeleton_node_path;
            break;
        }
        UtilityFunctions::push_warning(String("TressFXCharacter: could not resolve Skeleton3D from collision skeleton_node_path: ") + d.skeleton_node_path);
    }

    if (default_skeleton) {
        UtilityFunctions::print(
            String("TressFXCharacter: default skeleton resolved from collision node: path='") + default_skeleton_path +
            String("' bone_count=") + String::num_int64(default_skeleton->get_bone_count()));
    } else {
        UtilityFunctions::push_warning(
            "TressFXCharacter: no default Skeleton3D resolved from collision nodes (bone skinning will be unavailable)");
    }

    // Cache for Path A debug visualization: use the same authoritative skeleton.
    m_debug_skeleton = default_skeleton;
    m_debug_follow_bone = String();
    for (const auto &d : m_collisionDescriptions) {
        if (!d.followBone.is_empty()) {
            m_debug_follow_bone = d.followBone;
            break;
        }
    }

    m_hairStrands.clear();
    m_collisionMeshes.clear();
    m_adapterScenes.clear();

    UtilityFunctions::print(String("TressFXCharacter: creating adapter objects..."));

    for (int i = 0; i < (int)m_hairDescriptions.size(); ++i) {
        const auto& d = m_hairDescriptions[i];
        const CharString tfx = d.tfx_file.utf8();
        const CharString tfxbone = d.tfx_bone_file.utf8();
        const CharString obj = d.hair_object_name.utf8();

        UtilityFunctions::print(
            String("  HairStrands[") + String::num_int64(i) +
            String("] tfx='") + d.tfx_file +
            String("' bone='") + d.tfx_bone_file +
            String("' obj='") + d.hair_object_name +
            String("' follow=") + String::num_int64(d.num_follow_hairs) +
            String(" tip=") + String::num(d.tip_separation, 3) +
            String(" followRadius=") + String::num(d.follow_hair_radius, 4));

        m_adapterScenes.push_back(std::make_unique<EI_Scene>());
        m_adapterScenes.back()->set_skeleton(default_skeleton);

        UtilityFunctions::print(
            String("  HairStrands[") + String::num_int64(i) +
            String("] skeleton=") + (default_skeleton ? String("OK") : String("NULL")));

        m_hairStrands.push_back(std::make_unique<HairStrands>(
            /*scene=*/m_adapterScenes.back().get(),
            tfx.get_data(),
            tfxbone.get_data(),
            obj.get_data(),
            d.num_follow_hairs,
            d.tip_separation,
            d.follow_hair_radius,
            /*skinNumber=*/0,
            /*renderIndex=*/i));
    }

    for (int i = 0; i < (int)m_collisionDescriptions.size(); ++i) {
        const auto& d = m_collisionDescriptions[i];
        const CharString name = d.name.utf8();
        const CharString tfxmesh = d.tfx_mesh_file.utf8();
        const CharString follow = d.followBone.utf8();

        UtilityFunctions::print(
            String("  CollisionMesh[") + String::num_int64(i) +
            String("] name='") + d.name +
            String("' tfxmesh='") + d.tfx_mesh_file +
            String("' followBone='") + d.followBone +
            String("' cells=") + String::num_int64(d.numCellsInXAxis) +
            String(" margin=") + String::num(d.collisionMargin, 3));

        Skeleton3D* collision_skeleton = default_skeleton;
        if (!d.skeleton_node_path.is_empty()) {
            Node* node = get_node_or_null(NodePath(d.skeleton_node_path));
            Skeleton3D* sk = Object::cast_to<Skeleton3D>(node);
            if (sk) {
                collision_skeleton = sk;
            } else {
                UtilityFunctions::push_warning(String("TressFXCharacter: collision skeleton_node_path is not a Skeleton3D: ") + d.skeleton_node_path);
            }
        }

        m_adapterScenes.push_back(std::make_unique<EI_Scene>());
        m_adapterScenes.back()->set_skeleton(collision_skeleton);

        UtilityFunctions::print(
            String("  CollisionMesh[") + String::num_int64(i) +
            String("] skeleton=") + (collision_skeleton ? String("OK") : String("NULL")) +
            String(" (path='") + d.skeleton_node_path + String("')"));

        m_collisionMeshes.push_back(std::make_unique<CollisionMesh>(
            /*scene=*/m_adapterScenes.back().get(),
            /*renderPass=*/nullptr,
            name.get_data(),
            tfxmesh.get_data(),
            d.numCellsInXAxis,
            d.collisionMargin,
            /*skinNumber=*/0,
            follow.get_data()));
    }

    UtilityFunctions::print(
        String("TressFXCharacter: adapters created hair=") + String::num_int64((int)m_hairStrands.size()) +
        String(" coll=") + String::num_int64((int)m_collisionMeshes.size()));

    refresh_backend_mode();
    refresh_debug_hair_lines();
}

void TressFXCharacter::set_debug_draw_hair_lines(bool enabled) {
    m_debug_draw_hair_lines = enabled;
    const bool in_editor = (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint());

    // If toggled in the editor, ensure CPU-side assets are loaded so the debug mesh appears.
    if (m_debug_draw_hair_lines && in_editor) {
        if (m_hairStrands.empty()) {
            load_all_assets();
        }
    }

    refresh_backend_mode();
    refresh_debug_hair_lines();

    // Runtime mode switch: a live GPU runtime is torn down on the render thread,
    // which takes the hair objects with it — reload them for the new mode.
    if (!in_editor && m_hairStrands.empty() && !m_hairDescriptions.empty() && is_inside_tree()) {
        call_deferred("load_all_assets");
    }
}

bool TressFXCharacter::get_debug_draw_hair_lines() const {
    return m_debug_draw_hair_lines;
}

void TressFXCharacter::set_debug_max_guide_strands(int max_strands) {
    if (max_strands < 1) {
        max_strands = 1;
    }
    if (max_strands > 256) {
        max_strands = 256;
    }
    m_debug_max_guide_strands = max_strands;
    if (m_debug_draw_hair_lines) {
        refresh_debug_hair_lines();
    }
}

int TressFXCharacter::get_debug_max_guide_strands() const {
    return m_debug_max_guide_strands;
}

void TressFXCharacter::set_debug_hair_offset(const godot::Vector3& offset) {
    m_debug_hair_offset = offset;
    if (m_debug_draw_hair_lines) {
        // Update root transform immediately; mesh rebuild is not required.
        update_debug_hair_lines();
    }
}

godot::Vector3 TressFXCharacter::get_debug_hair_offset() const {
    return m_debug_hair_offset;
}

void TressFXCharacter::set_debug_hair_yaw_degrees(double degrees) {
    m_debug_hair_yaw_degrees = degrees;
    update_debug_hair_lines();
    update_gpu_debug_hair_lines_3d_transform();
}

double TressFXCharacter::get_debug_hair_yaw_degrees() const {
    return m_debug_hair_yaw_degrees;
}

void TressFXCharacter::set_show_gpu_debug_lines(bool enabled) {
    m_show_gpu_debug_lines = enabled;
    if (m_gpu_debug_line_instance) {
        m_gpu_debug_line_instance->set_visible(enabled);
    }
}

bool TressFXCharacter::get_show_gpu_debug_lines() const {
    return m_show_gpu_debug_lines;
}

void TressFXCharacter::set_gate_capture_mode(bool enabled) {
    m_gate_capture_mode = enabled;
}

bool TressFXCharacter::get_gate_capture_mode() const {
    return m_gate_capture_mode;
}

void TressFXCharacter::set_debug_force_fixed_dt(bool enabled) {
    m_debug_force_fixed_dt = enabled;
}

bool TressFXCharacter::get_debug_force_fixed_dt() const {
    return m_debug_force_fixed_dt;
}

void TressFXCharacter::set_wind_velocity(const godot::Vector3& wind_velocity) {
    m_wind_velocity = wind_velocity;
}

godot::Vector3 TressFXCharacter::get_wind_velocity() const {
    return m_wind_velocity;
}

void TressFXCharacter::set_gravity_magnitude(float v) { m_gravity_magnitude = v; }
float TressFXCharacter::get_gravity_magnitude() const { return m_gravity_magnitude; }

void TressFXCharacter::set_damping(float v) { m_damping = v; }
float TressFXCharacter::get_damping() const { return m_damping; }

void TressFXCharacter::set_global_stiffness(float v) { m_global_stiffness = v; }
float TressFXCharacter::get_global_stiffness() const { return m_global_stiffness; }

void TressFXCharacter::set_global_range(float v) { m_global_range = v; }
float TressFXCharacter::get_global_range() const { return m_global_range; }

void TressFXCharacter::set_local_stiffness(float v) { m_local_stiffness = v; }
float TressFXCharacter::get_local_stiffness() const { return m_local_stiffness; }

void TressFXCharacter::set_clamp_position_delta(float v) { m_clamp_position_delta = v; }
float TressFXCharacter::get_clamp_position_delta() const { return m_clamp_position_delta; }

void TressFXCharacter::set_collision_enabled(bool v) { m_collision_enabled = v; }
bool TressFXCharacter::get_collision_enabled() const { return m_collision_enabled; }

void TressFXCharacter::set_hair_fiber_radius(float v) {
    m_hair_fiber_radius = v;
    if (m_gpu_ribbon_material.is_valid()) {
        m_gpu_ribbon_material->set_shader_parameter("fiber_radius", m_hair_fiber_radius);
    }
}
float TressFXCharacter::get_hair_fiber_radius() const { return m_hair_fiber_radius; }

void TressFXCharacter::set_hair_root_color(const godot::Color& c) {
    m_hair_root_color = c;
    if (m_gpu_ribbon_material.is_valid()) {
        m_gpu_ribbon_material->set_shader_parameter("root_color", Vector3(c.r, c.g, c.b));
    }
}
godot::Color TressFXCharacter::get_hair_root_color() const { return m_hair_root_color; }

void TressFXCharacter::set_hair_tip_color(const godot::Color& c) {
    m_hair_tip_color = c;
    if (m_gpu_ribbon_material.is_valid()) {
        m_gpu_ribbon_material->set_shader_parameter("tip_color", Vector3(c.r, c.g, c.b));
    }
}
godot::Color TressFXCharacter::get_hair_tip_color() const { return m_hair_tip_color; }

void TressFXCharacter::rebuild_cpu_debug_visuals() {
    // Intended for editor usage via a @tool script.
    // Safe: only builds CPU-side meshes and Node3D children.
    if (!m_debug_draw_hair_lines) {
        return;
    }

    if (m_hairStrands.empty()) {
        load_all_assets();
    }

    refresh_debug_hair_lines();
    update_debug_hair_lines();
}

void TressFXCharacter::refresh_debug_hair_lines() {
    clear_debug_hair_lines();

    if (!m_debug_draw_hair_lines) {
        return;
    }

    // If assets haven't been loaded yet, do nothing; load_all_assets() will call refresh again.
    if (m_hairStrands.empty()) {
        return;
    }

    // Always parent debug visuals under the character so the editor shows an AABB
    // on the TressFXCharacter (what we relied on during earlier bring-up).
    // The debug root is moved to the follow-bone transform when available.
    Node3D* debug_root = memnew(Node3D);
    debug_root->set_name("TressFXDebugRoot");
    add_child(debug_root);
    m_debug_root = debug_root;

    m_debug_anchor = nullptr;
    if (m_debug_skeleton && !m_debug_follow_bone.is_empty()) {
        const int bone_idx = m_debug_skeleton->find_bone(m_debug_follow_bone);
        if (bone_idx >= 0) {
            BoneAttachment3D* attach = memnew(BoneAttachment3D);
            attach->set_name(String("TressFXDebugHairAnchor_") + m_debug_follow_bone);
            attach->set_bone_name(StringName(m_debug_follow_bone));
            m_debug_skeleton->add_child(attach);
            m_debug_anchor = attach;
        }
    }

    // Debug-only axis alignment: many TressFX assets are authored facing the opposite
    // direction relative to the Godot character forward.
    Transform3D t;
    if (m_debug_anchor) {
        t = m_debug_anchor->get_global_transform();
    } else if (m_debug_skeleton) {
        t = m_debug_skeleton->get_global_transform();
    } else {
        t = get_global_transform();
    }
    // Debug-only axis alignment (degrees).
    t.basis = t.basis * Basis(Vector3(0, 1, 0), (real_t)(Math_PI / 180.0) * (real_t)m_debug_hair_yaw_degrees);
    t.origin += t.basis.xform(m_debug_hair_offset);
    debug_root->set_global_transform(t);

    m_debug_lines_parent = debug_root;
    m_debug_line_instances.clear();

    const int cap = std::min(m_debug_max_guide_strands, 256);
    for (int i = 0; i < (int)m_hairStrands.size(); ++i) {
        Ref<ArrayMesh> mesh = m_hairStrands[i]->CreateDebugLineMesh(/*guides_only=*/true, /*strand_limit=*/cap);
        if (!mesh.is_valid()) {
            continue;
        }

        MeshInstance3D* mi = memnew(MeshInstance3D);
        mi->set_name(String("TressFXDebugLines_") + String::num_int64(i));
        mi->set_mesh(mesh);
        debug_root->add_child(mi);
        m_debug_line_instances.push_back(mi);
    }
}

void TressFXCharacter::refresh_gpu_debug_hair_lines_3d() {
    if (!m_gpu_mode_active) {
        return;
    }

    if (m_gpu_debug_root && m_gpu_debug_line_instance) {
        return;
    }

    // If assets haven't been loaded yet, do nothing; load_all_assets() will call refresh again.
    if (m_hairStrands.empty()) {
        return;
    }

    // Separate debug root for GPU-mode in-world visualization.
    Node3D* debug_root = memnew(Node3D);
    debug_root->set_name("TressFXGPUDebugRoot");
    add_child(debug_root);
    m_gpu_debug_root = debug_root;

    // No BoneAttachment3D anchor anymore: sim output is in skeleton model
    // space (bone motion lives in the skinning matrices), so the mount is the
    // Skeleton3D transform itself -- see update_gpu_debug_hair_lines_3d_transform.
    m_gpu_debug_anchor = nullptr;

    update_gpu_debug_hair_lines_3d_transform();

    if (!m_gpu_debug_lines_mesh.is_valid()) {
        m_gpu_debug_lines_mesh.instantiate();
    }

    MeshInstance3D* mi = memnew(MeshInstance3D);
    mi->set_name("TressFXGPUDebugLines_0");
    mi->set_mesh(m_gpu_debug_lines_mesh);

    // Simple bright unshaded material for visibility.
    Ref<StandardMaterial3D> mat;
    mat.instantiate();
    mat->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
    mat->set_albedo(Color(0.2f, 0.9f, 1.0f));
    mi->set_material_override(mat);
    // A2.4: the ribbon renderer is the real visual now; keep this hidden
    // unless explicitly re-enabled via the show_gpu_debug_lines property.
    mi->set_visible(m_show_gpu_debug_lines);

    debug_root->add_child(mi);
    m_gpu_debug_line_instance = mi;

    // A2.2: ribbon-expanded hair geometry, parented the same way so it follows
    // the same bone-anchored transform as the debug lines above.
    build_ribbon_mesh_if_needed();
    if (m_gpu_ribbon_mesh.is_valid()) {
        if (!m_gpu_ribbon_material.is_valid()) {
            m_gpu_ribbon_material.instantiate();
            Ref<Shader> shader = ResourceLoader::get_singleton()->load("res://shaders/hair_ribbon.gdshader");
            if (shader.is_valid()) {
                m_gpu_ribbon_material->set_shader(shader);
            } else {
                UtilityFunctions::push_warning("TressFXCharacter: failed to load res://shaders/hair_ribbon.gdshader");
            }
            m_gpu_ribbon_material->set_shader_parameter("vertices_per_strand", m_hairStrands[0]->GetVertsPerStrand());
            m_gpu_ribbon_material->set_shader_parameter("fiber_radius", m_hair_fiber_radius);
            m_gpu_ribbon_material->set_shader_parameter("root_color", Vector3(m_hair_root_color.r, m_hair_root_color.g, m_hair_root_color.b));
            m_gpu_ribbon_material->set_shader_parameter("tip_color", Vector3(m_hair_tip_color.r, m_hair_tip_color.g, m_hair_tip_color.b));
        }

        MeshInstance3D* ribbon_mi = memnew(MeshInstance3D);
        ribbon_mi->set_name("TressFXGPURibbon_0");
        ribbon_mi->set_mesh(m_gpu_ribbon_mesh);
        ribbon_mi->set_material_override(m_gpu_ribbon_material);
        // The mesh's "vertex" positions are placeholders overwritten entirely by
        // the vertex shader from the position texture, so Godot's automatic AABB
        // (derived from those placeholders) is meaningless -- without this, the
        // ribbons vanish whenever the placeholder-derived bounds leave the frustum.
        ribbon_mi->set_custom_aabb(AABB(Vector3(-2, -2, -2), Vector3(4, 4, 4)));
        debug_root->add_child(ribbon_mi);
        m_gpu_ribbon_instance = ribbon_mi;
    }
}

void TressFXCharacter::clear_gpu_debug_hair_lines_3d() {
    m_gpu_debug_line_instance = nullptr;
    m_gpu_debug_lines_mesh.unref();
    m_gpu_debug_root = nullptr;
    m_gpu_debug_anchor = nullptr;

    // The ribbon MeshInstance3D is a child of debug_root and gets freed by the
    // node-removal loop below; just drop our references to it.
    m_gpu_ribbon_instance = nullptr;
    m_gpu_ribbon_mesh.unref();
    m_gpu_ribbon_material.unref();

    // 1) Remove any GPU debug meshes attached directly under the character.
    for (int i = get_child_count() - 1; i >= 0; --i) {
        Node* c = get_child(i);
        if (!c) {
            continue;
        }
        const String n = String(c->get_name());
        if (n == "TressFXGPUDebugRoot" || n.begins_with("TressFXGPUDebugLines_")) {
            remove_child(c);
            c->queue_free();
        }
    }

    // 2) Remove any GPU debug attachments under the skeleton.
    if (m_debug_skeleton) {
        for (int i = m_debug_skeleton->get_child_count() - 1; i >= 0; --i) {
            Node* c = m_debug_skeleton->get_child(i);
            if (!c) {
                continue;
            }
            const String n = String(c->get_name());
            if (n.begins_with("TressFXGPUDebugHairAnchor_")) {
                m_debug_skeleton->remove_child(c);
                c->queue_free();
            }
        }
    }
}

void TressFXCharacter::update_gpu_debug_hair_lines_3d_transform() {
    if (!m_gpu_debug_root) {
        return;
    }

    // The sim outputs positions in SKELETON MODEL SPACE: the skinning matrices
    // (pose x rest^-1) already contain every bone's motion, including the root
    // bone's. So the only correct render mount is the Skeleton3D's own global
    // transform. Parenting to the root-bone BoneAttachment3D (the pre-A3 code)
    // applied the root bone's pose a SECOND time -- invisible while the
    // skeleton was never animated (an attachment on a never-dirtied skeleton
    // keeps its initial transform), but the first real pose write snapped the
    // anchor to the root joint's saved pose and teleported all the hair
    // (A3.1's "hair vanishes when the head shakes" was this, not a solver
    // explosion -- the watchdog showed perfectly healthy positions).
    Transform3D t;
    if (m_debug_skeleton) {
        t = m_debug_skeleton->get_global_transform();
    } else {
        t = get_global_transform();
    }

    // GPU mode should be driven by the skin/bone data (via HairStrands::UpdateBones)
    // and collision bindings. The CPU debug alignment knobs are visualization-only,
    // so we intentionally do NOT apply m_debug_hair_yaw_degrees / m_debug_hair_offset here.
    m_gpu_debug_root->set_global_transform(t);
}

void TressFXCharacter::update_gpu_debug_hair_lines_3d_mesh(const godot::PackedByteArray& pos_bytes, int vps, int guides) {
    if (vps < 2 || guides <= 0) {
        return;
    }
    if (!m_gpu_debug_lines_mesh.is_valid()) {
        m_gpu_debug_lines_mesh.instantiate();
    }

    const int segments_per_strand = vps - 1;
    const int64_t total_vertices = (int64_t)guides * (int64_t)segments_per_strand * 2;
    if (total_vertices <= 0) {
        return;
    }

    // Expect vec4 per vertex.
    const int64_t expected_bytes = (int64_t)guides * (int64_t)vps * 16;
    if ((int64_t)pos_bytes.size() < expected_bytes) {
        // Partial data: don't update mesh this frame.
        return;
    }

    godot::PackedVector3Array verts;
    verts.resize(total_vertices);

    const uint8_t* data = pos_bytes.ptr();
    auto read_vec3 = [&](int vertex_index) -> godot::Vector3 {
        const uint8_t* p = data + (size_t)vertex_index * 16;
        float fx = 0.0f, fy = 0.0f, fz = 0.0f;
        memcpy(&fx, p + 0, 4);
        memcpy(&fy, p + 4, 4);
        memcpy(&fz, p + 8, 4);
        return godot::Vector3(fx, fy, fz);
    };

    int64_t out_i = 0;
    for (int s = 0; s < guides; ++s) {
        const int base = s * vps;
        for (int v = 0; v < vps - 1; ++v) {
            const godot::Vector3 p0 = read_vec3(base + v);
            const godot::Vector3 p1 = read_vec3(base + v + 1);
            verts[(int)out_i++] = p0;
            verts[(int)out_i++] = p1;
        }
    }

    if (out_i != total_vertices) {
        verts.resize(out_i);
    }

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = verts;

    m_gpu_debug_lines_mesh->clear_surfaces();
    m_gpu_debug_lines_mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_LINES, arrays);
}

void TressFXCharacter::build_ribbon_mesh_if_needed() {
    // A2.2: static topology only -- no real positions here. Every vertex only
    // carries (global_vertex_index, side), packed into UV; the vertex shader
    // looks up the actual position from the A2.1 position texture every frame.
    // Built once per loaded asset (strand/vertex counts don't change at runtime).
    if (m_gpu_ribbon_mesh.is_valid() || m_hairStrands.empty() || !m_hairStrands[0]) {
        return;
    }

    HairStrands* h = m_hairStrands[0].get();
    const int vps = h->GetVertsPerStrand();
    const int total_strands = h->GetTotalStrandCount();
    if (vps < 2 || total_strands <= 0) {
        return;
    }

    const int segments_per_strand = vps - 1;
    const int64_t quad_count = (int64_t)total_strands * (int64_t)segments_per_strand;
    const int64_t vertex_count = quad_count * 4;
    const int64_t index_count = quad_count * 6;

    godot::PackedVector3Array placeholder_positions; // zero-filled; overwritten by the vertex shader
    placeholder_positions.resize(vertex_count);
    godot::PackedVector2Array vertex_uv; // x = global vertex index, y = side (0 -> -1, 1 -> +1)
    vertex_uv.resize(vertex_count);
    // Placeholder only: the shader fully overwrites TANGENT (and NORMAL/BINORMAL)
    // from the position texture every frame. Godot's renderer still requires the
    // mesh's vertex format to declare a tangent slot exists whenever the shader
    // writes TANGENT/BINORMAL, or it warns every frame ("requires tangents with
    // a mesh that doesn't contain tangents") -- these values are never read.
    godot::PackedFloat32Array vertex_tangents;
    vertex_tangents.resize(vertex_count * 4);
    for (int64_t t = 0; t < vertex_count; ++t) {
        vertex_tangents[t * 4 + 0] = 1.0f;
        vertex_tangents[t * 4 + 1] = 0.0f;
        vertex_tangents[t * 4 + 2] = 0.0f;
        vertex_tangents[t * 4 + 3] = 1.0f;
    }
    godot::PackedInt32Array indices;
    indices.resize(index_count);

    int64_t vi = 0;
    int64_t ii = 0;
    for (int s = 0; s < total_strands; ++s) {
        const int strand_base = s * vps;
        for (int local = 0; local < segments_per_strand; ++local) {
            const int v0 = strand_base + local;
            const int v1 = strand_base + local + 1;

            const int32_t quad_base = (int32_t)vi;
            vertex_uv[vi++] = godot::Vector2((float)v0, 0.0f); // A: v0, side -1
            vertex_uv[vi++] = godot::Vector2((float)v0, 1.0f); // B: v0, side +1
            vertex_uv[vi++] = godot::Vector2((float)v1, 0.0f); // C: v1, side -1
            vertex_uv[vi++] = godot::Vector2((float)v1, 1.0f); // D: v1, side +1

            // Quad ABDC split along the B-C diagonal.
            indices[ii++] = quad_base + 0;
            indices[ii++] = quad_base + 1;
            indices[ii++] = quad_base + 2;
            indices[ii++] = quad_base + 2;
            indices[ii++] = quad_base + 1;
            indices[ii++] = quad_base + 3;
        }
    }

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = placeholder_positions;
    arrays[godot::Mesh::ARRAY_TEX_UV] = vertex_uv;
    arrays[godot::Mesh::ARRAY_TANGENT] = vertex_tangents;
    arrays[godot::Mesh::ARRAY_INDEX] = indices;

    m_gpu_ribbon_mesh.instantiate();
    m_gpu_ribbon_mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_TRIANGLES, arrays);
}

void TressFXCharacter::clear_debug_hair_lines() {
    m_debug_line_instances.clear();
    m_debug_lines_parent = nullptr;
    m_debug_root = nullptr;
    m_debug_anchor = nullptr;

    // 1) Remove any debug meshes attached directly under the character.
    for (int i = get_child_count() - 1; i >= 0; --i) {
        Node* c = get_child(i);
        if (!c) {
            continue;
        }
        const String n = String(c->get_name());
        if (n == "TressFXDebugRoot" || n.begins_with("TressFXDebugLines_") || n.begins_with("TressFXDebugHairAttachment_")) {
            remove_child(c);
            c->queue_free();
        }
    }

    // 2) Remove any debug attachments under the skeleton (which also removes meshes parented to them).
    if (m_debug_skeleton) {
        for (int i = m_debug_skeleton->get_child_count() - 1; i >= 0; --i) {
            Node* c = m_debug_skeleton->get_child(i);
            if (!c) {
                continue;
            }
            const String n = String(c->get_name());
            if (n.begins_with("TressFXDebugHairAttachment_") || n.begins_with("TressFXDebugHairAnchor_")) {
                m_debug_skeleton->remove_child(c);
                c->queue_free();
            }
        }
    }
}

void TressFXCharacter::update_debug_hair_lines() {
    if (m_debug_root) {
        Transform3D t;
        if (m_debug_anchor) {
            t = m_debug_anchor->get_global_transform();
        } else if (m_debug_skeleton) {
            t = m_debug_skeleton->get_global_transform();
        } else {
            t = get_global_transform();
        }
        t.basis = t.basis * Basis(Vector3(0, 1, 0), (real_t)(Math_PI / 180.0) * (real_t)m_debug_hair_yaw_degrees);
        t.origin += t.basis.xform(m_debug_hair_offset);
        m_debug_root->set_global_transform(t);
    }
}

void TressFXCharacter::refresh_backend_mode() {
    const bool in_editor = (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint());

    if (in_editor) {
        // Editor safety: never initialize GPU/RenderingDevice.
        m_gpu_mode_active = false;
        teardown_gpu_runtime();
        clear_gpu_debug_hair_lines_3d();
        m_last_guide_positions_valid = false;
        update_process_state();
        return;
    }

    if (m_debug_draw_hair_lines) {
        // Debug/CPU mode: show CPU guide lines and keep GPU objects off.
        // NOTE: if the GPU runtime was live, teardown moves the hair objects to
        // the render thread for destruction; set_debug_draw_hair_lines reloads
        // assets afterwards so the CPU debug path has data again.
        m_gpu_mode_active = false;
        teardown_gpu_runtime();
        clear_gpu_debug_hair_lines_3d();
        m_last_guide_positions_valid = false;
        UtilityFunctions::print("TressFXCharacter: debug enabled -> CPU debug render (GPU disabled)");
    } else {
        // GPU mode: use Godot's main RenderingDevice (A1).
        // If shaders are missing, PSO creation will warn once per kernel and Simulation will remain inert.
        m_gpu_mode_active = true;
        teardown_gpu_runtime();
        m_last_guide_positions_valid = false;

        if (!GetDevice()) {
            UtilityFunctions::push_warning("TressFXCharacter: GPU mode requested but EI_Device is null");
        }

        UtilityFunctions::print("TressFXCharacter: debug disabled -> GPU mode (main-RD simulation + in-world guide debug)");
    }

    update_process_state();
}

godot::Ref<godot::Texture2DRD> TressFXCharacter::get_gpu_guide_lines_texture() {
    EI_Device* device = GetDevice();
    if (!device) {
        return godot::Ref<godot::Texture2DRD>();
    }

    // Request a one-shot overlay render on the next _process().
    // (Avoid calling RenderingDevice work from a getter; it may be invoked at unsafe times.)
    m_overlay_texture_requested = true;

    const godot::RID tex = device->GetMainRDGuideLinesTextureRID();
    if (!tex.is_valid()) {
        return godot::Ref<godot::Texture2DRD>();
    }

    if (!m_gpu_guide_lines_texture.is_valid()) {
        m_gpu_guide_lines_texture.instantiate();
    }
    m_gpu_guide_lines_texture->set_texture_rd_rid(tex);
    return m_gpu_guide_lines_texture;
}

godot::Ref<godot::Texture2DRD> TressFXCharacter::get_position_texture() {
    EI_Device* device = GetDevice();
    if (!device) {
        return godot::Ref<godot::Texture2DRD>();
    }

    // Pure state read (no RenderingDevice work): safe to call from any thread.
    // The texture itself is created/updated on the render thread by _rt_sim_tick.
    const godot::RID tex = device->GetPositionTextureRID();
    if (!tex.is_valid()) {
        return godot::Ref<godot::Texture2DRD>();
    }

    if (!m_position_texture.is_valid()) {
        m_position_texture.instantiate();
    }
    m_position_texture->set_texture_rd_rid(tex);
    return m_position_texture;
}

void TressFXCharacter::update_process_state() {
    // We need processing when either:
    // - CPU debug is active (to keep transforms updated), OR
    // - GPU mode is active (to tick simulation / future rendering).
    const bool in_editor = (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint());
    if (in_editor) {
        // In the editor, only tick when CPU debug is enabled.
        set_process(m_debug_draw_hair_lines);
    } else {
        set_process(m_debug_draw_hair_lines || m_gpu_mode_active);
    }
}

void TressFXCharacter::_notification(int what) {
    // Ensure the Texture2DRD wrapper does not hold onto a stale RID during shutdown.
    // This avoids exit-time invalid RID frees if the underlying main-RD resources
    // are cleaned up separately.
    const bool in_editor = (Engine::get_singleton() && Engine::get_singleton()->is_editor_hint());

    // Editor: ensure CPU debug meshes can appear even when this node is part of an instanced scene.
    if (in_editor && what == NOTIFICATION_ENTER_TREE) {
        if (m_debug_draw_hair_lines) {
            if (m_hairStrands.empty()) {
                load_all_assets();
            }
            refresh_debug_hair_lines();
        }
        update_process_state();
    }

    if (what == NOTIFICATION_EXIT_TREE || what == NOTIFICATION_PREDELETE) {
        if (m_gpu_guide_lines_texture.is_valid()) {
            m_gpu_guide_lines_texture->set_texture_rd_rid(RID());
            m_gpu_guide_lines_texture.unref();
        }
        if (m_position_texture.is_valid()) {
            m_position_texture->set_texture_rd_rid(RID());
            m_position_texture.unref();
        }

        // Ensure debug nodes/materials are released before teardown.
        clear_gpu_debug_hair_lines_3d();
        clear_debug_hair_lines();

        // A1: GPU-owned objects (main-RD buffers/PSOs) are destroyed on the
        // render thread, behind any sim tick still in flight.
        teardown_gpu_runtime();
    }
}
