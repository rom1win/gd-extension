#pragma once

#include <memory>
#include <string>

class EI_Scene;
class EI_CommandContext;
struct EI_RenderTargetSet;
class TressFXSDFCollisionSystem;
class TressFXHairObject;

// Godot-side adapter for the sample "CollisionMesh" glue type.
// Minimal placeholder so we can build and start wiring the scene-loading loop.
class CollisionMesh {
public:
    CollisionMesh(
        EI_Scene* scene,
        EI_RenderTargetSet* renderPass,
        const char* name,
        const char* tfxmeshFilePath,
        int numCellsInXAxis,
        float SDFCollMargin,
        int skinNumber,
        const char* followBone);

    void SkinTheMesh(EI_CommandContext& context, double fTime);
    void AccumulateSDF(EI_CommandContext& context, TressFXSDFCollisionSystem& sdfCollisionSystem);
    void ApplySDF(EI_CommandContext& context, TressFXSDFCollisionSystem& sdfCollisionSystem, TressFXHairObject* strands);

    void GenerateIsoSurface(EI_CommandContext& context);
    void DrawIsoSurface(EI_CommandContext& context);
    void DrawMesh(EI_CommandContext& context);

private:
    // Stored for debugging and future implementation work.
    EI_Scene* m_pScene = nullptr;
    std::string m_name;
    std::string m_tfxmeshFilePath;
    std::string m_followBone;
    int m_numCellsInXAxis = 0;
    float m_SDFCollMargin = 0.0f;
    int m_skinNumber = 0;
};
