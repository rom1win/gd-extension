#include "tfx_bridge.h"
#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void TFXHairs::_bind_methods() {
}

TFXHairs::TFXHairs() {
	// Initialize any variables here.
	time_passed = 0.0;
}

TFXHairs::~TFXHairs() {
	// Add your cleanup here.
}

void TFXHairs::_process(double delta) {
	time_passed += delta;

	Vector3 new_position = Vector3(10.0 + (10.0 * sin(time_passed * 2.0)), 10.0 + (10.0 * cos(time_passed * 1.5)), 10.0 + (10.0 * tan(time_passed * 2.0)));

	set_position(new_position);
}