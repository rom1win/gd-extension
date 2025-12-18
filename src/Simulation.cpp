#include "Simulation.h"
#include "GodotEngineInterfaceImpl.h"
#include "TressFXLayouts.h"

#include <atomic>
#include <godot_cpp/variant/utility_functions.hpp>

static std::atomic<int> g_tressfx_layout_refcount = 0;

Simulation::Simulation() {
    m_tressFXSimulation = std::make_unique<TressFXSimulation>();
}

Simulation::~Simulation() {
    if (m_acquired_global_layouts) {
        const int after = --g_tressfx_layout_refcount;
        if (after == 0) {
            if (EI_Device* pDevice = GetDevice()) {
                DestroyAllLayouts(pDevice);
            }
        }
        m_acquired_global_layouts = false;
    }
}

void Simulation::Initialize() {
    EI_Device* pDevice = GetDevice(); 
    godot::UtilityFunctions::print(godot::String("Simulation: Initialize() device=") + (pDevice ? "non-null" : "null"));

    if (!pDevice) {
        return;
    }

    // TressFX core expects global reusable layouts to exist before any PSO creation.
    // Without this, GetSimLayout()/GetSimPosTanLayout() dereference a null global.
    if (!m_acquired_global_layouts) {
        const int before = g_tressfx_layout_refcount.fetch_add(1);
        if (before == 0) {
            InitializeAllLayouts(pDevice);
        }
        m_acquired_global_layouts = true;
    }

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
