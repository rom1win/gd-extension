#pragma once

#include <string>

class EI_Scene;
struct EI_RenderTargetSet;

// Godot-side adapter for the sample "CollisionMesh" glue type.
// Collision/SDF is deferred (see CLAUDE.md); this node currently only records
// its configuration and lets TressFXCharacter resolve a skeleton from it.
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

    ~CollisionMesh();

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
