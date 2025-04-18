#ifndef TFX_BRIDGE_H
#define TFX_BRIDGE_H

#include <godot_cpp/classes/sprite2d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>

namespace godot {

class TFXHairs : public MeshInstance3D {
	GDCLASS(TFXHairs, MeshInstance3D)

private:
	double time_passed;

protected:
	static void _bind_methods();

public:
	TFXHairs();
	~TFXHairs();

	void _process(double delta) override;
};

}

#endif