#include "tressfx_character.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/bone_attachment3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
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

    ClassDB::bind_method(D_METHOD("get_gpu_guide_lines_texture"), &TressFXCharacter::get_gpu_guide_lines_texture);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "gpu_guide_lines_texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "", "get_gpu_guide_lines_texture");
}

TressFXCharacter::TressFXCharacter() {}

TressFXCharacter::~TressFXCharacter() {}

void TressFXCharacter::_init() {}

void TressFXCharacter::_ready() {
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
    load_all_assets();

    update_process_state();
}

void TressFXCharacter::_process(double delta) {
    m_frame_index++;
    m_time_seconds += delta;

    if (m_debug_draw_hair_lines) {
        update_debug_hair_lines();
    }

    if (!m_pSimulation) {
        return;
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

    // Until EI_Device and command submission are real, this is a safe no-op.
    m_pSimulation->StartSimulation(m_time_seconds, ctx, /*bUpdateCollMesh=*/false, /*bSDFCollisionResponse=*/false, /*bAsync=*/false);
    m_pSimulation->WaitOnSimulation();
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
    // Keep the earlier bring-up alignment: 90 degrees around Y.
    t.basis = t.basis * Basis(Vector3(0, 1, 0), Math_PI * 0.5);
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
        t.basis = t.basis * Basis(Vector3(0, 1, 0), Math_PI * 0.5);
        t.origin += t.basis.xform(m_debug_hair_offset);
        m_debug_root->set_global_transform(t);
    }
}

void TressFXCharacter::refresh_backend_mode() {
    if (m_debug_draw_hair_lines) {
        // Debug/CPU mode: show CPU guide lines and keep GPU objects off.
        m_gpu_mode_active = false;
        m_pPPLL.reset();
        m_pShortCut.reset();
        m_pSimulation.reset();
        UtilityFunctions::print("TressFXCharacter: debug enabled -> CPU debug render (GPU disabled)");
    } else {
        // GPU mode: use Godot RenderingDevice.
        // Run RenderingDevice self-tests, then initialize Simulation.
        // If shaders are missing, PSO creation will warn once per kernel and Simulation will remain inert.
        m_gpu_mode_active = true;
        m_pPPLL.reset();
        m_pShortCut.reset();
        m_pSimulation.reset();

        if (EI_Device* device = GetDevice()) {
            device->RunSelfTestOnce();
            device->RunMainRDSelfTestOnce();
            device->RunMainRDImageSelfTestOnce();
        } else {
            UtilityFunctions::push_warning("TressFXCharacter: GPU mode requested but EI_Device is null");
        }

        // Safe even if shaders are missing (no crash; missing PSOs are handled).
        m_pSimulation = std::make_unique<Simulation>();
        m_pSimulation->Initialize();

        // Milestone 3-5: minimal GPU guide-line render.
        // Feed guide positions (from the first hair object) to EI_Device and request a one-shot render.
        if (!m_hairStrands.empty()) {
            godot::PackedByteArray pos_bytes;
            int vps = 0;
            int guides = 0;
            if (m_hairStrands[0]->PackGuidePositionsVec4(pos_bytes, vps, guides)) {
                UtilityFunctions::print(String("TressFXCharacter: GuideLines source set guides=") + String::num_int64(guides) +
                                        String(" vps=") + String::num_int64(vps) +
                                        String(" bytes=") + String::num_int64(pos_bytes.size()));
                if (EI_Device* device = GetDevice()) {
                    device->SetGuideLinesSource(pos_bytes, vps, guides);
                    device->RunMainRDGuideLinesOnce();
                }
            } else {
                UtilityFunctions::push_warning("TressFXCharacter: GuideLines source unavailable (asset not loaded?)");
            }
        }

        UtilityFunctions::print("TressFXCharacter: debug disabled -> GPU mode (RenderingDevice + Simulation init; missing shaders will disable GPU work)");
    }

    update_process_state();
}

godot::Ref<godot::Texture2DRD> TressFXCharacter::get_gpu_guide_lines_texture() {
    EI_Device* device = GetDevice();
    if (!device) {
        return godot::Ref<godot::Texture2DRD>();
    }

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
    set_process(m_debug_draw_hair_lines || m_gpu_mode_active);
}
