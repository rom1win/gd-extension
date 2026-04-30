#pragma once

#include "TressFX/TressFXSimulation.h"
#include <memory>
#include <vector>

#include <godot_cpp/variant/vector3.hpp>

// Godot-compatible replacement for the TressFX sample's Simulation class.
// Wraps the core TressFXSimulation class.

class HairStrands;
class CollisionMesh;

struct SimulationContext {
    std::vector<HairStrands*> hairStrands;
    std::vector<CollisionMesh*> collisionMeshes;

    // Wind in world space (direction * magnitude). Default zero = no wind.
    godot::Vector3 wind_velocity = godot::Vector3(0, 0, 0);

    // Simulation tuning — RatBoy defaults from TressFXSample.cpp.
    float vspCoeff                    = 0.758f;
    float vspAccelThreshold           = 1.208f;
    float localConstraintStiffness    = 0.908f;
    int   localConstraintsIterations  = 3;
    float globalConstraintStiffness   = 0.408f;  // KEY: must be > 0 to anchor hair to skeleton
    float globalConstraintsRange      = 0.308f;  // KEY: must be > 0 (shader guards both)
    int   lengthConstraintsIterations = 3;
    float damping                     = 0.068f;
    float gravityMagnitude            = 0.09f;
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
