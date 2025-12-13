#ifndef TRESSFX_CHARACTER_H
#define TRESSFX_CHARACTER_H

#include <memory>
#include <vector>

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include "tressfx_node.h"
#include "Simulation.h"
#include "HairStrands.h"
#include "SDF.h"
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
    void _process(double delta);

    // Registration API used by child hair/collision nodes
    void register_hair_description(const TressFXHairNode::TressFXObjectDescription &desc);
    void register_collision_description(const TressFXHairNode::TressFXCollisionMeshDescription &desc);

    // Trigger creation of the TressFX runtime objects (stub for now)
    void load_all_assets();

private:
    std::vector<TressFXHairNode::TressFXObjectDescription> m_hairDescriptions;
    std::vector<TressFXHairNode::TressFXCollisionMeshDescription> m_collisionDescriptions;

    std::vector<std::unique_ptr<HairStrands>> m_hairStrands;
    std::vector<std::unique_ptr<CollisionMesh>> m_collisionMeshes;

    std::unique_ptr<Simulation> m_pSimulation;
    std::unique_ptr<TressFXPPLL> m_pPPLL;
    std::unique_ptr<TressFXShortCut> m_pShortCut;

    double m_time_seconds = 0.0;
    uint64_t m_frame_index = 0;
    double m_last_tick_log_time_seconds = -1.0;
};

#endif // TRESSFX_CHARACTER_H
