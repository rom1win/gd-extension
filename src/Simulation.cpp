#include "Simulation.h"
#include "GodotEngineInterfaceImpl.h"
#include "TressFXLayouts.h"
#include "TressFX/TressFXHairObject.h"
#include "TressFX/TressFXSettings.h"
#include "HairStrands.h"
#include "SDF.h"

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
    // Treat fTime as timestep seconds (passed from Godot's _process(delta)).
    (void)bSDFCollisionResponse;
    (void)bAsync;

    EI_Device* device = GetDevice();
    if (!device) {
        return;
    }

    // A1: this returns the MAIN RenderingDevice; we are on the render thread.
    godot::RenderingDevice* rd = device->GetLocalRenderingDevice();
    if (!rd) {
        return;
    }

    EI_CommandContext& commandContext = device->GetCurrentCommandContext();
    commandContext.set_rd(rd);

    // Ensure hair objects exist and gather handles.
    std::vector<TressFXHairObject*> hairObjects;
    hairObjects.reserve(ctx.hairStrands.size());
    for (HairStrands* h : ctx.hairStrands) {
        if (!h) {
            continue;
        }
        if (!h->EnsureTressFXObjectCreated()) {
            continue;
        }
        TressFXHairObject* ho = h->GetTressFXHandle();
        if (ho) {
            hairObjects.push_back(ho);
        }
    }

    if (hairObjects.empty()) {
        return;
    }

    const float dt = (float)fTime;
    const float timeStep = (dt > 0.0f) ? std::min(std::max(dt, 1.0f / 240.0f), 1.0f / 15.0f) : (1.0f / 60.0f);

    // Simulation settings tuned to the original RatBoy sample values (TressFXSample.cpp).
    // The critical ones are globalConstraintStiffness/Range: without them the global shape
    // constraint block in the integration shader never fires and hair floats free of the skeleton.
    TressFXSimulationSettings settings{};
    settings.m_vspCoeff                   = ctx.vspCoeff;
    settings.m_vspAccelThreshold          = ctx.vspAccelThreshold;
    settings.m_localConstraintStiffness   = ctx.localConstraintStiffness;
    settings.m_localConstraintsIterations = ctx.localConstraintsIterations;
    settings.m_globalConstraintStiffness  = ctx.globalConstraintStiffness;
    settings.m_globalConstraintsRange     = ctx.globalConstraintsRange;
    settings.m_lengthConstraintsIterations= ctx.lengthConstraintsIterations;
    settings.m_damping                    = ctx.damping;
    settings.m_gravityMagnitude           = ctx.gravityMagnitude;
    settings.m_tipSeparation              = ctx.tipSeparation;
    settings.m_clampPositionDelta         = ctx.clampPositionDelta;
    {
        const godot::Vector3 w = ctx.wind_velocity;
        const float mag = (float)w.length();
        settings.m_windMagnitude = mag;

        if (mag > 0.0001f) {
            const godot::Vector3 dir = w / mag;
            settings.m_windDirection[0] = (float)dir.x;
            settings.m_windDirection[1] = (float)dir.y;
            settings.m_windDirection[2] = (float)dir.z;
        } else {
            // Direction is unused when magnitude is zero, but keep it deterministic.
            settings.m_windDirection[0] = 1.0f;
            settings.m_windDirection[1] = 0.0f;
            settings.m_windDirection[2] = 0.0f;
        }
    }

    // Update per-object inputs.
    // Bones come pre-snapshotted from the main thread (ctx.bone_matrices) —
    // never touch Skeleton3D here. They land in the CPU-side constant buffer,
    // uploaded by UpdateConstantBuffer inside Simulate().
    for (size_t i = 0; i < ctx.hairStrands.size(); ++i) {
        HairStrands* h = ctx.hairStrands[i];
        if (!h || !h->GetTressFXHandle()) {
            continue;
        }
        if (i < ctx.bone_matrices.size() && ctx.bone_matrices[i].size() > 0) {
            h->ApplyBoneMatricesBytes(ctx.bone_matrices[i]);
        }
        h->GetTressFXHandle()->UpdateSimulationParameters(&settings, timeStep);
        h->TransitionRenderingToSim(commandContext);
    }

    // A3.2 subtask 2: skin the collision mesh(es) to the current bone pose
    // BEFORE the hair kernels run (bSDFCollisionResponse stays false this
    // subtask -- nothing consumes the skinned buffer on the hair side yet;
    // this only keeps it up to date for verification/subtask 3+).
    if (bUpdateCollMesh) {
        for (size_t i = 0; i < ctx.collisionMeshes.size(); ++i) {
            CollisionMesh* c = ctx.collisionMeshes[i];
            if (!c || !c->IsValid()) {
                continue;
            }
            if (i < ctx.collision_bone_matrices.size() && ctx.collision_bone_matrices[i].size() > 0) {
                c->UpdateSkinning(commandContext, ctx.collision_bone_matrices[i]);
            }
        }
    }

    // Run the simulation kernels.
    m_tressFXSimulation->Simulate(commandContext, hairObjects);

    // Transition back so other consumers can read positions as SRV (future rendering).
    for (HairStrands* h : ctx.hairStrands) {
        if (!h || !h->GetTressFXHandle()) {
            continue;
        }
        h->TransitionSimToRendering(commandContext);
    }

    // Close the compute list. On the main RD this never submits or syncs —
    // the engine owns the frame (the per-frame GPU stall is gone).
    device->EndAndSubmitCommandBuffer();
    m_simulationRunning = true;
}

void Simulation::WaitOnSimulation() {
    // Increment 1: no-op (we run synchronously).
    m_simulationRunning = false;
}
