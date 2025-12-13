#include "HairStrands.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include "TressFX/TressFXHairObject.h"

HairStrands::~HairStrands() = default;

HairStrands::HairStrands(
    EI_Scene* scene,
    const char* tfxFilePath,
    const char* tfxboneFilePath,
    const char* hairObjectName,
    int numFollowHairsPerGuideHair,
    float tipSeparationFactor,
    int skinNumber,
    int renderIndex)
    : m_pScene(scene),
      m_skinNumber(skinNumber),
      m_tfxFilePath(tfxFilePath ? tfxFilePath : ""),
      m_tfxboneFilePath(tfxboneFilePath ? tfxboneFilePath : ""),
      m_hairObjectName(hairObjectName ? hairObjectName : ""),
      m_numFollowHairsPerGuideHair(numFollowHairsPerGuideHair),
      m_tipSeparationFactor(tipSeparationFactor),
      m_renderIndex(renderIndex) {
    // First incremental implementation:
    // Do not create TressFXHairObject yet (it needs a real EI_Device and command context).
    // This keeps runtime safe while we bring up the rendering backend.

        godot::UtilityFunctions::print(
                godot::String("HairStrands: constructed renderIndex=") + godot::String::num_int64(m_renderIndex) +
                godot::String(" skin=") + godot::String::num_int64(m_skinNumber) +
                godot::String(" tfx='") + godot::String(m_tfxFilePath.c_str()) +
                godot::String("' bone='") + godot::String(m_tfxboneFilePath.c_str()) +
                godot::String("' obj='") + godot::String(m_hairObjectName.c_str()) +
                godot::String("'"));
}

void HairStrands::TransitionSimToRendering(EI_CommandContext& context) {
    if (!m_pStrands) {
        // No GPU resources yet.
        return;
    }
    m_pStrands->GetDynamicState().TransitionSimToRendering(context);
}

void HairStrands::TransitionRenderingToSim(EI_CommandContext& context) {
    if (!m_pStrands) {
        // No GPU resources yet.
        return;
    }
    m_pStrands->GetDynamicState().TransitionRenderingToSim(context);
}

void HairStrands::UpdateBones(EI_CommandContext& context) {
    (void)context;
    // Intentionally no-op in this incremental step.
    // Once EI_Scene exposes bone matrices in a TressFX-friendly format, we will:
    // - query skeleton matrices
    // - call m_pStrands->UpdateBoneMatrices(...)
}
