#pragma once

#include "TressFX/TressFXSimulation.h"
#include <memory>
#include <vector>

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

// Godot-compatible replacement for the TressFX sample's Simulation class.
// Wraps the core TressFXSimulation class.
//
// A1 threading contract: Initialize() and StartSimulation() run ONLY on the
// render thread (scheduled via RenderingServer::call_on_render_thread by
// TressFXCharacter). They never touch scene nodes; bone matrices arrive
// pre-snapshotted by value in SimulationContext::bone_matrices.

class HairStrands;
class CollisionMesh;

// A3.2 subtask B: one capsule collider, already resolved to SKELETON MODEL
// SPACE (same frame as the bone skinning matrices) on the MAIN thread by
// TressFXCollisionNode::get_capsule_data before it is snapshotted here.
struct CollisionCapsuleData {
    godot::Vector3 center_a;
    float radius_a = 0.0f;
    godot::Vector3 center_b;
    float radius_b = 0.0f;
};

struct SimulationContext {
    std::vector<HairStrands*> hairStrands;
    std::vector<CollisionMesh*> collisionMeshes;

    // Per-hair skeleton snapshot (AMD::float4x4 array bytes, pose x rest^-1,
    // packing per NOTES.md §1), captured on the MAIN thread. Empty entry =
    // keep the previously applied matrices.
    std::vector<godot::PackedByteArray> bone_matrices;

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
    float tipSeparation               = 1.0f;   // follow-hair spread; RatBoy default (NOTES §6)
    // Max distance (sim-space units = meters for us) a vertex may move per sim
    // step before the kernel clamps it and rewrites its velocity history.
    // AMD's default 20 assumed a centimeter-scale world; in meters it never
    // fires. Kept at 20 by default (= AMD behavior, gate-identical); lower it
    // to engage the clamp (A3.1 fast-bone-motion stability).
    float clampPositionDelta          = 20.0f;

    // Empty when TressFXCharacter::collision_enabled is false. Capped at
    // TRESSFX_MAX_NUM_COLLISION_CAPSULES (8) by the caller.
    std::vector<CollisionCapsuleData> collisionCapsules;
};

class Simulation {
public:
    Simulation();
    ~Simulation();

    // Render thread only: compiles the kernel PSOs on the main RD.
    void Initialize();
    // Render thread only: applies bones/params and dispatches the kernel chain
    // on the main RD. Ends the compute list; never submits or syncs.
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
