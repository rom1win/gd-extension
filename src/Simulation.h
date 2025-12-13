#pragma once

#include "TressFX/TressFXSimulation.h"
#include <memory>
#include <vector>

// Godot-compatible replacement for the TressFX sample's Simulation class.
// Wraps the core TressFXSimulation class.

class Simulation {
public:
    Simulation();
    ~Simulation();

    void Initialize();
    // TODO: Add Update/Simulate/Draw methods as we implement the runtime logic

private:
    std::unique_ptr<TressFXSimulation> m_tressFXSimulation;
};
