#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

class EI_Scene;
struct EI_RenderTargetSet;
class EI_Resource;
class EI_PSO;
class EI_BindSet;
class EI_CommandContext;

// Godot-side adapter for the sample "CollisionMesh" glue type.
// A3.2 subtask 1: CPU parsing of the AMD .tfxmesh text format (see
// thirdparty/tressfx/src/TressFX/TressFXBoneSkinning.cpp
// LoadTressFXCollisionMeshData) plus the GPU storage buffers its Init() flow
// expects.
// A3.2 subtask 2 added the bone-skinning kernel (UpdateSkinning) so the mesh
// follows the skeleton; SDF generation/marching-cubes/collision response are
// still not wired up (later subtasks).
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

    bool IsValid() const { return m_valid; }
    int GetNumVertices() const { return m_numVertices; }
    int GetNumTriangles() const { return m_numTriangles; }

    // RENDER thread only (mirrors HairStrands::EnsureTressFXObjectCreated):
    // creates the main-RD storage buffers from the already-parsed CPU data
    // and uploads the rest-pose data. Safe to call repeatedly; only the
    // first successful call does work. Returns false if parsing failed.
    bool EnsureGPUResourcesCreated();

    // A3.2 subtask 2: bone-skinning kernel (follows the same threading split
    // as HairStrands::SnapshotBoneMatrices/ApplyBoneMatricesBytes).
    // MAIN thread: snapshot the skeleton's skinning matrices as raw
    // AMD::float4x4 bytes (touches Skeleton3D only, via m_pScene).
    godot::PackedByteArray SnapshotBoneMatrices() const;

    // RENDER thread: lazily creates the skinning PSO/bind set/UBO on first
    // call, then uploads bone_matrices_snapshot and dispatches BoneSkinning
    // (ceil(numVertices/64) groups), followed by a UAV barrier. No-op if the
    // mesh failed to load or GPU resources are not ready yet.
    void UpdateSkinning(EI_CommandContext& commandContext, const godot::PackedByteArray& bone_matrices_snapshot);

    // RID of the skinned output buffer (interleaved position+normal, same
    // layout as the rest-pose SRV). Invalid until EnsureGPUResourcesCreated().
    godot::RID GetSkinnedPositionsRID() const;

    // CPU-only: rest-pose position for vertex i (bounds-checked). Used by the
    // one-shot skin-check readback to print a rest/skinned comparison.
    godot::Vector3 GetRestPosition(int i) const;

    // CPU reference implementation of the GLSL BoneSkinning kernel's math
    // (weighted-blend up to 4 bone matrices, same clamp/zero-pad rules),
    // used ONLY by the one-shot skin-check verification -- never on the
    // simulation-critical path. MUST be called with the exact same
    // bone_matrices_snapshot bytes handed to UpdateSkinning() for the same
    // tick, or the comparison is meaningless under animation.
    godot::Vector3 CpuSkinVertex(int vertexIndex, const godot::PackedByteArray& bone_matrices_snapshot) const;

private:
    // RENDER thread: creates m_boneMatrixUBO / m_boneSkinningBindSet /
    // m_boneSkinningPSO on first call. Requires EnsureGPUResourcesCreated()
    // and the global TressFX layouts (GetBoneSkinningMeshLayout()) to already
    // exist. Safe to call repeatedly.
    bool EnsureSkinningPSOCreated();
    // Matches thirdparty/tressfx/src/TressFX/TressFXAsset.h's
    // TressFXBoneSkinningData layout (float[4] boneIndex + float[4] weight)
    // byte-for-byte, without pulling that header into this widely-included one.
    struct VertexBoneData {
        float boneIndex[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float weight[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    };

    // MAIN thread, CPU-only: parses the .tfxmesh text file (Godot FileAccess,
    // res:// aware) and resolves bone names through m_pScene, same mechanism
    // HairStrands uses for .tfxbone names.
    bool LoadTfxMesh();

    // Stored for debugging and future implementation work.
    EI_Scene* m_pScene = nullptr;
    std::string m_name;
    std::string m_tfxmeshFilePath;
    std::string m_followBone;
    int m_numCellsInXAxis = 0;
    float m_SDFCollMargin = 0.0f;
    int m_skinNumber = 0;

    // CPU-side parsed data (rest pose).
    std::vector<godot::Vector3> m_positions;
    std::vector<godot::Vector3> m_normals;
    std::vector<VertexBoneData> m_boneData;
    std::vector<int32_t> m_indices; // numTriangles * 3
    int m_numVertices = 0;
    int m_numTriangles = 0;
    int m_numBonesInFile = 0;
    bool m_valid = false;

    godot::Vector3 m_aabbMin;
    godot::Vector3 m_aabbMax;

    // GPU resources (created lazily by EnsureGPUResourcesCreated, render thread only).
    bool m_gpuResourcesCreated = false;
    std::unique_ptr<EI_Resource> m_restPositionsNormalsSRV; // read-only: interleaved rest position+normal
    std::unique_ptr<EI_Resource> m_boneSkinningDataSRV;     // read-only: per-vertex bone indices+weights
    std::unique_ptr<EI_Resource> m_skinnedPositionsUAV;     // UAV-capable: skinned output (seeded with rest pose)
    std::unique_ptr<EI_Resource> m_triangleIndicesSRV;      // triangle index buffer

    // Bone-skinning kernel (created lazily by EnsureSkinningPSOCreated, render thread only).
    bool m_skinningPSOCreated = false;
    std::unique_ptr<EI_Resource> m_boneMatrixUBO;
    std::unique_ptr<EI_BindSet> m_boneSkinningBindSet;
    std::unique_ptr<EI_PSO> m_boneSkinningPSO;
};
