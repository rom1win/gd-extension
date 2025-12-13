#include "tressfx_character.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include "tressfx_collision_node.h"
#include "Simulation.h"

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

    m_pSimulation.reset(new Simulation);
}
