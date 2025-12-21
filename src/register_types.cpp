#include "register_types.h"

#include "tressfx_node.h"
#include "tressfx_character.h"
#include "tressfx_collision_node.h"

#include "GodotEngineInterfaceImpl.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include <cstdio>

using namespace godot;

void initialize_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	godot::UtilityFunctions::print("tfx_bridge: initialize_module(MODULE_INITIALIZATION_LEVEL_SCENE)");

	// EI_Device holds Godot Variant types and must be created after godot-cpp init.
	InitializeGodotEngineInterface();

	// Register the new TressFX node wrappers
	GDREGISTER_CLASS(TressFXHairNode);
	GDREGISTER_CLASS(TressFXCollisionNode);
	GDREGISTER_CLASS(TressFXCharacter);
}

void uninitialize_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ShutdownGodotEngineInterface();
}

extern "C" {
// Initialization.
GDExtensionBool GDE_EXPORT tfx_bridge_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	std::printf("tfx_bridge: tfx_bridge_init() called\n");
	std::fflush(stdout);

	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	std::printf("tfx_bridge: InitObject created\n");
	std::fflush(stdout);

	init_obj.register_initializer(initialize_module);
	init_obj.register_terminator(uninitialize_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	GDExtensionBool ok = init_obj.init();
	std::printf("tfx_bridge: init_obj.init() returned %d\n", (int)ok);
	std::fflush(stdout);
	return ok;
}
}
