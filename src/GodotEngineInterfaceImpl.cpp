#include "EngineInterface.h"

static EI_Device g_Device;

EI_Device* GetDevice() {
    return &g_Device;
}
