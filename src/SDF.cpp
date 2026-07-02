#include "SDF.h"

#include <godot_cpp/variant/utility_functions.hpp>

CollisionMesh::~CollisionMesh() = default;

CollisionMesh::CollisionMesh(
    EI_Scene* scene,
    EI_RenderTargetSet* renderPass,
    const char* name,
    const char* tfxmeshFilePath,
    int numCellsInXAxis,
    float SDFCollMargin,
    int skinNumber,
    const char* followBone)
    : m_pScene(scene),
      m_name(name ? name : ""),
      m_tfxmeshFilePath(tfxmeshFilePath ? tfxmeshFilePath : ""),
      m_followBone(followBone ? followBone : ""),
      m_numCellsInXAxis(numCellsInXAxis),
      m_SDFCollMargin(SDFCollMargin),
      m_skinNumber(skinNumber) {
    (void)renderPass;

    godot::UtilityFunctions::print(
            godot::String("CollisionMesh: constructed name='") + godot::String(m_name.c_str()) +
            godot::String("' tfxmesh='") + godot::String(m_tfxmeshFilePath.c_str()) +
            godot::String("' followBone='") + godot::String(m_followBone.c_str()) +
            godot::String("' cells=") + godot::String::num_int64(m_numCellsInXAxis) +
            godot::String(" margin=") + godot::String::num(m_SDFCollMargin, 3));

    godot::UtilityFunctions::print("CollisionMesh: collision-mesh loading disabled (deferred until the collision phase)");
}
