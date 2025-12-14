#include "tressfx_character.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
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
}

TressFXCharacter::TressFXCharacter() {}

TressFXCharacter::~TressFXCharacter() {}

void TressFXCharacter::_init() {}

void TressFXCharacter::_ready() {
    // One-shot compute backend sanity check.
    if (EI_Device* device = GetDevice()) {
        device->RunSelfTestOnce();
        device->RunMainRDSelfTestOnce();
    }

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

    // CPU-only bring-up: keep per-frame simulation disabled until the GPU backend
    // (PPLL/ShortCut/Simulation) is fully implemented.
    set_process(false);
}

void TressFXCharacter::_process(double delta) {
    m_frame_index++;
    m_time_seconds += delta;

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

    // GPU bring-up is still in progress. PPLL/ShortCut/Simulation currently require
    // texture/image resources that aren't implemented in EI_Device yet.
    // Keep this CPU-only to avoid crashes.
    m_pPPLL.reset();
    m_pShortCut.reset();
    m_pSimulation.reset();
    UtilityFunctions::print("TressFXCharacter: GPU simulation disabled (EI_Device incomplete)");

    refresh_debug_hair_lines();
}

void TressFXCharacter::set_debug_draw_hair_lines(bool enabled) {
    m_debug_draw_hair_lines = enabled;
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

void TressFXCharacter::refresh_debug_hair_lines() {
    // Remove previous debug children (if any).
    for (int i = get_child_count() - 1; i >= 0; --i) {
        Node* c = get_child(i);
        if (c && String(c->get_name()).begins_with("TressFXDebugLines")) {
            remove_child(c);
            c->queue_free();
        }
    }

    if (!m_debug_draw_hair_lines) {
        return;
    }

    // If assets haven't been loaded yet, do nothing; load_all_assets() will call refresh again.
    if (m_hairStrands.empty()) {
        return;
    }

    const int cap = std::min(m_debug_max_guide_strands, 256);
    for (int i = 0; i < (int)m_hairStrands.size(); ++i) {
        Ref<ArrayMesh> mesh = m_hairStrands[i]->CreateDebugLineMesh(/*guides_only=*/true, /*strand_limit=*/cap);
        if (!mesh.is_valid()) {
            continue;
        }

        MeshInstance3D* mi = memnew(MeshInstance3D);
        mi->set_name(String("TressFXDebugLines_") + String::num_int64(i));
        mi->set_mesh(mesh);
        add_child(mi);
    }
}
