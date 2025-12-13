#include "Simulation.h"
#include "GodotEngineInterfaceImpl.h"
#include <godot_cpp/variant/utility_functions.hpp>

Simulation::Simulation() {
    m_tressFXSimulation = std::make_unique<TressFXSimulation>();
}

Simulation::~Simulation() {
}

void Simulation::Initialize() {
    EI_Device* pDevice = GetDevice(); 
    godot::UtilityFunctions::print(godot::String("Simulation: Initialize() device=") + (pDevice ? "non-null" : "null"));
    m_tressFXSimulation->Initialize(pDevice);
}

void Simulation::StartSimulation(
    double fTime,
    SimulationContext& ctx,
    bool bUpdateCollMesh,
    bool bSDFCollisionResponse,
    bool bAsync) {
    (void)fTime;
    (void)ctx;
    (void)bUpdateCollMesh;
    (void)bSDFCollisionResponse;
    (void)bAsync;
    // Increment 1: no-op.
    // This becomes the home for:
    // - transitioning hair to sim
    // - running TressFXSimulation steps
    // - optional collision/SDF steps
    m_simulationRunning = true;
    // Keep this extremely light to avoid log spam. The caller (TressFXCharacter) already logs ticks.
}

void Simulation::WaitOnSimulation() {
    // Increment 1: no-op (we run synchronously).
    m_simulationRunning = false;
}
