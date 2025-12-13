#include "tressfx_character.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include "tressfx_collision_node.h"
#include "Simulation.h"
#include "HairStrands.h"
#include "SDF.h"

using namespace godot;

void TressFXCharacter::_bind_methods() {
    // Only expose load_all_assets to scripting for now. The register_* methods are
    // C++-only and accept POD structs that are not bindable to Variant automatically.
    ClassDB::bind_method(D_METHOD("load_all_assets"), &TressFXCharacter::load_all_assets);
    // No properties for now
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

    // Increment 1: enable per-frame ticking. The simulation path is still stubbed,
    // but this wires the call order and ensures nothing crashes when enabled.
    set_process(true);
}

void TressFXCharacter::_process(double delta) {
    m_frame_index++;
    m_time_seconds += delta;

    if (!m_pSimulation) {
        if ((m_frame_index % 120) == 0) {
            UtilityFunctions::print(String("TressFXCharacter: tick (no simulation yet) t=") + String::num(m_time_seconds));
        }
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

    // Throttled tick log (about 2x per second at 60 fps).
    if (m_last_tick_log_time_seconds < 0.0 || (m_time_seconds - m_last_tick_log_time_seconds) >= 0.5) {
        m_last_tick_log_time_seconds = m_time_seconds;
        UtilityFunctions::print(
            String("TressFXCharacter: tick t=") + String::num(m_time_seconds, 3) +
            String(" hair=") + String::num_int64((int)ctx.hairStrands.size()) +
            String(" coll=") + String::num_int64((int)ctx.collisionMeshes.size()));
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
    UtilityFunctions::print(String("TressFXCharacter: registered collision: ") + desc.tfx_mesh_file);
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

    // Increment 1: create adapter objects, but do not allocate GPU resources yet.
    // This makes the LoadScene loop shape testable without a full EI_Device.
    m_hairStrands.clear();
    m_collisionMeshes.clear();

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

        m_hairStrands.push_back(std::make_unique<HairStrands>(
            /*scene=*/nullptr,
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

        m_collisionMeshes.push_back(std::make_unique<CollisionMesh>(
            /*scene=*/nullptr,
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

    m_pPPLL.reset(new TressFXPPLL);
    m_pShortCut.reset(new TressFXShortCut);
    m_pSimulation.reset(new Simulation);

    int width = 1920;
    int height = 1080;

    if (is_inside_tree()) {
        Viewport* vp = get_viewport();
        if (vp) {
            Vector2i size = vp->call("get_size");
            width = size.x;
            height = size.y;
        }
    }

    UtilityFunctions::print(String("TressFXCharacter: viewport size ") + String::num_int64(width) + String("x") + String::num_int64(height));

    // Initialize PPLL
    // nNodes and nodeSize.
    // TressFX sample uses:
    // int nNodes = width * height * 8; // Average 8 layers?
    // int nodeSize = TRESSFX_DEFAULT_NODE_SIZE;
    
    int nNodes = width * height * 16; // Let's be generous
    UtilityFunctions::print(String("TressFXCharacter: initializing PPLL nNodes=") + String::num_int64(nNodes));
    m_pPPLL->Initialize(width, height, nNodes, TRESSFX_DEFAULT_NODE_SIZE);
    
    // Initialize ShortCut
    UtilityFunctions::print(String("TressFXCharacter: initializing ShortCut"));
    m_pShortCut->Initialize(width, height);
    
    // Initialize Simulation
    UtilityFunctions::print(String("TressFXCharacter: initializing Simulation"));
    m_pSimulation->Initialize();
}
