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
class TressFXHairObject;

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
        const char* followBone,
        int sdfPaddingCells = 40);

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

    // A3.2 subtask 3: builds the per-frame signed-distance grid from the
    // freshly skinned collision mesh (InitializeSignedDistanceField ->
    // barrier -> ConstructSignedDistanceField -> barrier ->
    // FinalizeSignedDistanceField -> barrier, mirroring
    // thirdparty/tressfx/src/TressFX/TressFXSDFCollision.cpp Update()).
    // RENDER thread only; lazily creates the grid buffer/UBO/PSOs on first
    // call (requires EnsureGPUResourcesCreated() to already have run; safe
    // no-op otherwise). Call AFTER UpdateSkinning() for this same mesh/tick,
    // with a barrier already submitted (UpdateSkinning does this) so
    // ConstructSignedDistanceField reads the freshly skinned vertices.
    // bone_matrices_snapshot: the SAME bytes passed to UpdateSkinning() this
    // tick -- used only to re-center the grid on the mesh's current rigid
    // position via the follow bone (see .cpp for why this, not a GPU
    // bounding-box readback, matches AMD's own scheme).
    void UpdateSDF(EI_CommandContext& commandContext, const godot::PackedByteArray& bone_matrices_snapshot);

    // RID of the uint32 signed-distance grid buffer. Holds plain IEEE-754
    // float bit patterns once FinalizeSignedDistanceField has run (bit-cast,
    // not FloatFlip-encoded -- Finalize's whole job is undoing that
    // encoding). Invalid until the first UpdateSDF() call.
    godot::RID GetSDFGridRID() const;
    int GetSDFNumTotalCells() const { return m_numTotalCells; }
    int GetSDFNumCellsX() const { return m_numCellsX; }
    int GetSDFNumCellsY() const { return m_numCellsY; }
    int GetSDFNumCellsZ() const { return m_numCellsZ; }
    float GetSDFCellSize() const { return m_cellSize; }
    // Grid origin computed by the most recent UpdateSDF() call (see
    // m_lastGridOrigin). Debug/authoring use only (voxel visualization) --
    // travels with async readbacks so late-arriving data is placed using the
    // grid snapshot it actually reflects, not wherever the grid is by the
    // time the readback completes.
    godot::Vector3 GetSDFGridOrigin() const { return m_lastGridOrigin; }

    // A3.2 subtask 4: SDF-vs-hair collision response. Faithful port of
    // thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl entry point
    // `CollideHairVerticesWithSdf_forward` (~532) -- the ONE AMD's own sample
    // dispatches (see TressFXSDFCollision.cpp CollideWithHair() /
    // TressFXSDFCollision.h's Initialize(), which compiles that exact entry
    // point despite the misleadingly-named PSO variable). RENDER thread only;
    // requires UpdateSDF() to have already run THIS tick for this mesh (reuses
    // the grid origin/cellSize/dims it just computed -- no extra barrier is
    // needed beyond what UpdateSDF already leaves behind, since this only
    // READS the finalized grid). Writes directly into `hair`'s
    // Positions/PositionsPrev buffers (its ApplySDF bind set, created
    // automatically by the vendored TressFXHairObject::CreateGPUResources) --
    // physics-affecting, only ever called when sdf_collision_enabled is on.
    void CollideWithHair(EI_CommandContext& commandContext, TressFXHairObject* hair);

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

    // RENDER thread: computes the SDF grid's cell size/dimensions ONCE from
    // the mesh's rest-pose AABB (faithful port of
    // TressFXSDFCollision::TressFXSDFCollision's constructor math), then
    // creates the grid buffer, params UBO, bind set (GetGenerateSDFLayout(),
    // already vendored in TressFXLayouts.cpp) and the three build PSOs.
    // Requires EnsureGPUResourcesCreated() to already have run. Safe to call
    // repeatedly.
    bool EnsureSDFPSOCreated();

    // RENDER thread: creates m_collidePSO on first call (requires
    // EnsureSDFPSOCreated() to have already run: reuses m_sdfBindSet as set 0
    // and adds the vendored ApplySDFLayout as set 1). Safe to call repeatedly.
    bool EnsureCollidePSOCreated();

    // Matches thirdparty/tressfx/src/TressFX/TressFXConstantBuffers.h's
    // TressFXSDFCollisionParams / TressFXSDFCollision.hlsl's ConstBuffer_SDF
    // byte-for-byte (std140: a leading vec4 then 12 tightly-packed 4-byte
    // scalars = 64 bytes, no implicit padding).
    struct SDFParamsUBOData {
        float origin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float cellSize = 0.0f;
        int32_t numCellsX = 0;
        int32_t numCellsY = 0;
        int32_t numCellsZ = 0;
        int32_t maxMarchingCubesVertices = 0;
        float marchingCubesIsolevel = 0.0f;
        float collisionMargin = 0.0f;
        int32_t numHairVerticesPerStrand = 0;
        int32_t numTotalHairVertices = 0;
        float pad1 = 0.0f;
        float pad2 = 0.0f;
        float pad3 = 0.0f;
    };
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
    // SDF grid padding (cells per side, all 3 axes) -- see EnsureSDFPSOCreated.
    // Debug/authoring tunable (SDF follow-up); default 40 matches AMD's own
    // 0.8*numCellsInXAxis derivation at this scene's default numCellsInXAxis=50.
    int m_sdfPaddingCells = 40;

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

    // Bone index of m_followBone within m_skinNumber's skeleton, resolved
    // once at load time (same GetBoneIdByName mechanism as per-vertex bone
    // indices). Used only to re-center the SDF grid every frame (see
    // UpdateSDF); falls back to 0 (root) if the name doesn't resolve, same
    // convention as per-vertex bone indices.
    int m_followBoneIndex = 0;

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

    // SDF grid (A3.2 subtask 3). Dimensions/cell size are computed ONCE from
    // the rest-pose AABB (AMD's TressFXSDFCollision constructor scheme, see
    // EnsureSDFPSOCreated); only the grid ORIGIN is recomputed every
    // UpdateSDF() call, tracking the mesh's rigid motion via the follow bone
    // (AMD's TressFXBoneSkinning::GetBoundingBox() scheme -- no GPU
    // bounding-box readback exists in either AMD's sample or our port).
    bool m_sdfPSOCreated = false;
    float m_cellSize = 0.0f;
    int m_numCellsX = 0;
    int m_numCellsY = 0;
    int m_numCellsZ = 0;
    int m_numTotalCells = 0;
    godot::Vector3 m_paddingBoundary;

    std::unique_ptr<EI_Resource> m_sdfGridUAV; // uint32[numTotalCells], see GetSDFGridRID()
    std::unique_ptr<EI_Resource> m_sdfParamsUBO;
    std::unique_ptr<EI_BindSet> m_sdfBindSet;
    std::unique_ptr<EI_PSO> m_initSdfPSO;
    std::unique_ptr<EI_PSO> m_constructSdfPSO;
    std::unique_ptr<EI_PSO> m_finalizeSdfPSO;

    // Grid origin computed by THIS tick's UpdateSDF() call, reused verbatim by
    // CollideWithHair() (same tick, same bone snapshot -- see UpdateSDF's own
    // comment on why origin is the only per-frame-varying grid field).
    godot::Vector3 m_lastGridOrigin;

    // Collide-hair kernel (A3.2 subtask 4, created lazily by
    // EnsureCollidePSOCreated, render thread only). Reuses m_sdfParamsUBO/
    // m_sdfBindSet (set 0); set 1 is a per-hair-object ApplySDF bind set owned
    // by the TressFXHairObject itself (GetDynamicState().GetApplySDFBindSet()).
    bool m_collidePSOCreated = false;
    std::unique_ptr<EI_PSO> m_collidePSO;
};
