#include "Simulation.h"
#include "GodotEngineInterfaceImpl.h"

Simulation::Simulation() {
    m_tressFXSimulation = std::make_unique<TressFXSimulation>();
}

Simulation::~Simulation() {
}

void Simulation::Initialize() {
    EI_Device* pDevice = GetDevice(); 
    m_tressFXSimulation->Initialize(pDevice);
}
