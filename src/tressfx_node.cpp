#include "tressfx_node.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/variant/array.hpp>
#include <vector>
#include "tressfx_character.h"

using namespace godot;

void TressFXHairNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("load_tfx_asset"), &TressFXHairNode::load_tfx_asset);
    ClassDB::bind_method(D_METHOD("bind_to_godot_mesh", "mesh_node_path"), &TressFXHairNode::bind_to_godot_mesh);

    // Bind property setters/getters so ADD_PROPERTY can find them
    ClassDB::bind_method(D_METHOD("set_tfx_file", "p"), &TressFXHairNode::set_tfx_file);
    ClassDB::bind_method(D_METHOD("get_tfx_file"), &TressFXHairNode::get_tfx_file);

    ClassDB::bind_method(D_METHOD("set_tfx_bone_file", "p"), &TressFXHairNode::set_tfx_bone_file);
    ClassDB::bind_method(D_METHOD("get_tfx_bone_file"), &TressFXHairNode::get_tfx_bone_file);

    ClassDB::bind_method(D_METHOD("set_hair_object_name", "p"), &TressFXHairNode::set_hair_object_name);
    ClassDB::bind_method(D_METHOD("get_hair_object_name"), &TressFXHairNode::get_hair_object_name);

    ClassDB::bind_method(D_METHOD("set_mesh_surface_index", "p"), &TressFXHairNode::set_mesh_surface_index);
    ClassDB::bind_method(D_METHOD("get_mesh_surface_index"), &TressFXHairNode::get_mesh_surface_index);

    ClassDB::bind_method(D_METHOD("set_num_follow_hairs", "p"), &TressFXHairNode::set_num_follow_hairs);
    ClassDB::bind_method(D_METHOD("get_num_follow_hairs"), &TressFXHairNode::get_num_follow_hairs);

    ClassDB::bind_method(D_METHOD("set_tip_separation", "p"), &TressFXHairNode::set_tip_separation);
    ClassDB::bind_method(D_METHOD("get_tip_separation"), &TressFXHairNode::get_tip_separation);

    // Properties
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "tfx_file", PROPERTY_HINT_FILE, "*.tfx,*.tfxbone"), "set_tfx_file", "get_tfx_file");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "tfx_bone_file", PROPERTY_HINT_FILE, "*.tfxbone"), "set_tfx_bone_file", "get_tfx_bone_file");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "hair_object_name"), "set_hair_object_name", "get_hair_object_name");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "mesh_surface_index"), "set_mesh_surface_index", "get_mesh_surface_index");
    ADD_PROPERTY(PropertyInfo(Variant::INT, "num_follow_hairs"), "set_num_follow_hairs", "get_num_follow_hairs");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tip_separation"), "set_tip_separation", "get_tip_separation");

    // Editor helper: expose a method that returns available .tfx files
    ClassDB::bind_method(D_METHOD("find_tfx_files"), &TressFXHairNode::find_tfx_files);
}

TressFXHairNode::TressFXHairNode() {
}

TressFXHairNode::~TressFXHairNode() {
}

void TressFXHairNode::_init() {
    // default values already inlined in declaration
}

void TressFXHairNode::_ready() {
    // On ready, attempt to find a parent TressFXCharacter and register our description.
    // We only register a minimal description; the character will own the runtime objects.
    Node *p = get_parent();
    UtilityFunctions::print(String("TressFXHairNode::_ready() values: tfx_file='") + tfx_file + String("' tfx_bone_file='") + tfx_bone_file + String("' hair_object_name='") + hair_object_name);
    while (p) {
        TressFXCharacter *character = Object::cast_to<TressFXCharacter>(p);
        if (character) {
            // Make sure our local description is populated
            last_object_description.name = hair_object_name.is_empty() ? String("tfx_object") : hair_object_name;
            last_object_description.tfx_file = tfx_file;
            last_object_description.tfx_bone_file = tfx_bone_file;
            last_object_description.hair_object_name = hair_object_name;
            last_object_description.mesh_surface_index = mesh_surface_index;
            last_object_description.num_follow_hairs = num_follow_hairs;
            last_object_description.tip_separation = tip_separation;

            // Only register if we have a tfx file set; otherwise skip to avoid empty entries.
            if (!last_object_description.tfx_file.is_empty()) {
                character->register_hair_description(last_object_description);
            } else {
                UtilityFunctions::print(String("TressFXHairNode: tfx_file is empty; skipping registration."));
            }
            break;
        }
        p = p->get_parent();
    }
}

void TressFXHairNode::load_tfx_asset() {
    // Populate our local lightweight description structures using the inspector values.
    last_object_description.name = hair_object_name.is_empty() ? String("tfx_object") : hair_object_name;
    last_object_description.tfx_file = tfx_file;
    last_object_description.tfx_bone_file = tfx_bone_file;
    last_object_description.hair_object_name = hair_object_name;
    last_object_description.mesh_surface_index = mesh_surface_index;
    last_object_description.num_follow_hairs = num_follow_hairs;
    last_object_description.tip_separation = tip_separation;

    // For this first step we also create a minimal collision description (empty/default)
    last_collision_description.name = last_object_description.name + String("_collision");
    last_collision_description.tfx_mesh_file = String();
    last_collision_description.numCellsInXAxis = 0;
    last_collision_description.collisionMargin = 0.0f;
    last_collision_description.mesh = 0;
    last_collision_description.followBone = String();

    // Inform the user (print to Godot output)
    UtilityFunctions::print(String("TressFXNode: created object description: ") + last_object_description.tfx_file);
    UtilityFunctions::print(String("TressFXNode: bone file: ") + last_object_description.tfx_bone_file);
}

void TressFXHairNode::bind_to_godot_mesh(const NodePath &mesh_node_path) {
    target_mesh_path = mesh_node_path;
    UtilityFunctions::print(String("TressFXNode: bound to mesh path: ") + String(mesh_node_path));
}

void TressFXHairNode::register_to_character(TressFXCharacter *character) {
    // Ensure our description is populated, then call into the character.
    load_tfx_asset();
    if (character) {
        if (!last_object_description.tfx_file.is_empty()) {
            character->register_hair_description(last_object_description);
        } else {
            UtilityFunctions::print(String("TressFXHairNode::register_to_character: tfx_file empty; skipping registration."));
        }
    }
}

Array TressFXHairNode::find_tfx_files() {
    Array results;

    // stack-based recursion to avoid deep recursion
    std::vector<String> dirs;
    dirs.push_back(String("res://"));

    while (!dirs.empty()) {
        String dirpath = dirs.back();
        dirs.pop_back();

        Ref<DirAccess> da = DirAccess::open(dirpath);
        if (da.is_null()) {
            continue;
        }

        da->list_dir_begin();
        while (true) {
            String name = da->get_next();
            if (name == String()) break;
            if (name == String(".") || name == String("..")) continue;

            String full = dirpath;
            if (!full.ends_with("/"))
                full += String("/");
            full += name;

            if (da->current_is_dir()) {
                dirs.push_back(full);
            } else {
                String lname = name.to_lower();
                if (lname.ends_with(".tfx") || lname.ends_with(".tfxbone")) {
                    results.push_back(full);
                }
            }
        }
        da->list_dir_end();
    }

    return results;
}
