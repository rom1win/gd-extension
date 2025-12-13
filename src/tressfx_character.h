#ifndef TRESSFX_CHARACTER_H
#define TRESSFX_CHARACTER_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include "tressfx_node.h"
#include "Simulation.h"
#include "TressFX/TressFXPPLL.h"
#include "TressFX/TressFXShortCut.h"

using namespace godot;

class TressFXCharacter : public Node3D {
    GDCLASS(TressFXCharacter, Node3D)

public:
    TressFXCharacter();
    ~TressFXCharacter();

    static void _bind_methods();

    void _init();
    void _ready();

    // Registration API used by child hair/collision nodes
    void register_hair_description(const TressFXHairNode::TressFXObjectDescription &desc);
    void register_collision_description(const TressFXHairNode::TressFXCollisionMeshDescription &desc);

    // Trigger creation of the TressFX runtime objects (stub for now)
    void load_all_assets();

private:
    std::vector<TressFXHairNode::TressFXObjectDescription> m_hairDescriptions;
    std::vector<TressFXHairNode::TressFXCollisionMeshDescription> m_collisionDescriptions;
    std::unique_ptr<Simulation> m_pSimulation;
    std::unique_ptr<TressFXPPLL> m_pPPLL;
    std::unique_ptr<TressFXShortCut> m_pShortCut;
};

#endif // TRESSFX_CHARACTER_H
