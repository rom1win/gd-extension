#pragma once

#include "TressFX/TressFXSimulation.h"
#include <memory>
#include <vector>

// Godot-compatible replacement for the TressFX sample's Simulation class.
// Wraps the core TressFXSimulation class.

class HairStrands;
class CollisionMesh;

struct SimulationContext {
    std::vector<HairStrands*> hairStrands;
    std::vector<CollisionMesh*> collisionMeshes;
};

class Simulation {
public:
    Simulation();
    ~Simulation();

    void Initialize();
    void StartSimulation(
        double fTime,
        SimulationContext& ctx,
        bool bUpdateCollMesh,
        bool bSDFCollisionResponse,
        bool bAsync);
    void WaitOnSimulation();

private:
    std::unique_ptr<TressFXSimulation> m_tressFXSimulation;
    bool m_simulationRunning = false;
    bool m_acquired_global_layouts = false;
};
