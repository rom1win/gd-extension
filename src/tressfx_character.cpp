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
#include "tressfx_collision_node.h"
#include "EngineInterface.h"
#include "Simulation.h"
#include "HairStrands.h"
#include "SDF.h"
#include "GodotScene.h"

using namespace godot;

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

    ClassDB::bind_method(D_METHOD("set_wind_velocity", "wind_velocity"), &TressFXCharacter::set_wind_velocity);
    ClassDB::bind_method(D_METHOD("get_wind_velocity"), &TressFXCharacter::get_wind_velocity);
    ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind_velocity"), "set_wind_velocity", "get_wind_velocity");

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

    ClassDB::bind_method(D_METHOD("rebuild_cpu_debug_visuals"), &TressFXCharacter::rebuild_cpu_debug_visuals);

    // Keep as an explicit method for optional overlay viewers; do not expose as an inspector property.
    ClassDB::bind_method(D_METHOD("get_gpu_guide_lines_texture"), &TressFXCharacter::get_gpu_guide_lines_texture);
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

    if (m_gpu_mode_active) {
        update_gpu_debug_hair_lines_3d_transform();
    }

    SimulationContext ctx;
    ctx.hairStrands.reserve(m_hairStrands.size());
    for (auto& h : m_hairStrands) {
        ctx.hairStrands.push_back(h.get());
    }

    ctx.collisionMeshes.reserve(m_collisionMeshes.size());
    for (auto& c : m_collisionMeshes) {
        ctx.collisionMeshes.push_back(c.get());
    }

    ctx.wind_velocity             = m_wind_velocity;
    ctx.gravityMagnitude          = m_gravity_magnitude;
    ctx.damping                   = m_damping;
    ctx.globalConstraintStiffness = m_global_stiffness;
    ctx.globalConstraintsRange    = m_global_range;
    ctx.localConstraintStiffness  = m_local_stiffness;

    // Late init GPU mode at first runtime tick. This avoids touching RenderingDevice too early
    // during scene initialization and prevents startup crashes.
    if (m_gpu_mode_active && !m_gpu_runtime_initialized) {
        if (!GetDevice()) {
            return;
        }
        m_pSimulation = std::make_unique<Simulation>();
        m_pSimulation->Initialize();

        for (auto& h : m_hairStrands) {
            if (h) {
                h->EnsureTressFXObjectCreated();
            }
        }
        m_gpu_runtime_initialized = true;
    }

    // Simulation (local-RD bring-up): run compute kernels and update GPU positions.
    if (m_pSimulation) {
        m_pSimulation->StartSimulation(delta, ctx, /*bUpdateCollMesh=*/false, /*bSDFCollisionResponse=*/false, /*bAsync=*/false);
        m_pSimulation->WaitOnSimulation();
    }

    // GPU guide-lines:
    // - If a GPU hair object exists, read back simulated guide positions.
    // - Otherwise, fall back to CPU-skinned guide positions.
    if (m_gpu_mode_active) {
        if (EI_Device* device = GetDevice()) {
            if (!m_hairStrands.empty()) {
                godot::PackedByteArray pos_bytes;
                int vps = 0;
                int guides = 0;

                bool ok = false;
                if (m_hairStrands[0]->GetTressFXHandle()) {
                    ok = m_hairStrands[0]->PackSimulatedGuidePositionsVec4(pos_bytes, vps, guides, m_debug_max_guide_strands);
                }
                if (!ok) {
                    ok = m_hairStrands[0]->PackGuidePositionsVec4(pos_bytes, vps, guides);
                }

                if (ok) {
                    // Cache for optional overlay output.
                    m_last_guide_positions_bytes = pos_bytes;
                    m_last_vertices_per_strand = vps;
                    m_last_guide_strands = guides;
                    m_last_guide_positions_valid = true;

                    // In-world 3D debug lines (always on in GPU mode at this milestone).
                    refresh_gpu_debug_hair_lines_3d();
                    update_gpu_debug_hair_lines_3d_mesh(pos_bytes, vps, guides);
                } else {
                    m_last_guide_positions_valid = false;
                }
            }

            // Optional legacy overlay render: only execute when requested by a UI viewer.
            if (m_overlay_texture_requested && m_last_guide_positions_valid) {
                device->SetGuideLinesSource(m_last_guide_positions_bytes, m_last_vertices_per_strand, m_last_guide_strands);
                device->RunMainRDGuideLinesOnce();
            }
        }
        m_overlay_texture_requested = false;
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
            String(" tip=") + String::num(d.tip_separation, 3));

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

    // If toggled in the editor, ensure CPU-side assets are loaded so the debug mesh appears.
    if (m_debug_draw_hair_lines && Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()) {
        if (m_hairStrands.empty()) {
            load_all_assets();
        }
    }

    refresh_backend_mode();
    refresh_debug_hair_lines();
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

    m_gpu_debug_anchor = nullptr;
    if (m_debug_skeleton && !m_debug_follow_bone.is_empty()) {
        const int bone_idx = m_debug_skeleton->find_bone(m_debug_follow_bone);
        if (bone_idx >= 0) {
            BoneAttachment3D* attach = memnew(BoneAttachment3D);
            attach->set_name(String("TressFXGPUDebugHairAnchor_") + m_debug_follow_bone);
            attach->set_bone_name(StringName(m_debug_follow_bone));
            m_debug_skeleton->add_child(attach);
            m_gpu_debug_anchor = attach;
        }
    }

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

    debug_root->add_child(mi);
    m_gpu_debug_line_instance = mi;
}

void TressFXCharacter::clear_gpu_debug_hair_lines_3d() {
    m_gpu_debug_line_instance = nullptr;
    m_gpu_debug_lines_mesh.unref();
    m_gpu_debug_root = nullptr;
    m_gpu_debug_anchor = nullptr;

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

    Transform3D t;
    if (m_gpu_debug_anchor) {
        t = m_gpu_debug_anchor->get_global_transform();
    } else if (m_debug_skeleton) {
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
        m_gpu_runtime_initialized = false;
        m_pPPLL.reset();
        m_pShortCut.reset();
        m_pSimulation.reset();
        clear_gpu_debug_hair_lines_3d();
        m_last_guide_positions_valid = false;
        update_process_state();
        return;
    }

    if (m_debug_draw_hair_lines) {
        // Debug/CPU mode: show CPU guide lines and keep GPU objects off.
        m_gpu_mode_active = false;
        m_gpu_runtime_initialized = false;
        m_pPPLL.reset();
        m_pShortCut.reset();
        m_pSimulation.reset();
        clear_gpu_debug_hair_lines_3d();
        m_last_guide_positions_valid = false;
        UtilityFunctions::print("TressFXCharacter: debug enabled -> CPU debug render (GPU disabled)");
    } else {
        // GPU mode: use Godot RenderingDevice.
        // Initialize Simulation and run the current GPU milestone (simulation + in-world guide-line debug).
        // If shaders are missing, PSO creation will warn once per kernel and Simulation will remain inert.
        m_gpu_mode_active = true;
        m_pPPLL.reset();
        m_pShortCut.reset();
        m_pSimulation.reset();
        m_last_guide_positions_valid = false;

        if (!GetDevice()) {
            UtilityFunctions::push_warning("TressFXCharacter: GPU mode requested but EI_Device is null");
        }

        // Defer simulation + GPU object creation until the first _process().
        m_gpu_runtime_initialized = false;

        UtilityFunctions::print("TressFXCharacter: debug disabled -> GPU mode (simulation + in-world guide debug)");
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

        // Ensure debug nodes/materials are released before teardown.
        clear_gpu_debug_hair_lines_3d();
        clear_debug_hair_lines();
    }
}
