#pragma once

#include <memory>
#include <string>

class EI_Scene;
class EI_CommandContext;
class TressFXHairObject;

// Godot-side adapter for the sample "HairStrands" glue type.
// Intentionally minimal for now: it provides the API shape used by TressFX core
// (PPLL/ShortCut draw paths) and by our future scene-loading loop.
class HairStrands {
public:
    HairStrands(
        EI_Scene* scene,
        const char* tfxFilePath,
        const char* tfxboneFilePath,
        const char* hairObjectName,
        int numFollowHairsPerGuideHair,
        float tipSeparationFactor,
        int skinNumber,
        int renderIndex);

    ~HairStrands();

    TressFXHairObject* GetTressFXHandle() const { return m_pStrands.get(); }

    void TransitionSimToRendering(EI_CommandContext& context);
    void TransitionRenderingToSim(EI_CommandContext& context);
    void UpdateBones(EI_CommandContext& context);

private:
    std::unique_ptr<TressFXHairObject> m_pStrands;
    EI_Scene* m_pScene = nullptr;
    int m_skinNumber = 0;

    // Stored for debugging and future initialization work.
    std::string m_tfxFilePath;
    std::string m_tfxboneFilePath;
    std::string m_hairObjectName;
    int m_numFollowHairsPerGuideHair = 0;
    float m_tipSeparationFactor = 1.0f;
    int m_renderIndex = 0;
};
