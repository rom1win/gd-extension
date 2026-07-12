#pragma once

#include <memory>
#include <string>

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

class EI_Scene;
class EI_CommandContext;
class TressFXHairObject;
class TressFXAsset;

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

    // Creates the GPU-backed TressFX hair object (local RenderingDevice) if needed.
    // Returns true if the object exists after the call.
    bool EnsureTressFXObjectCreated();

    void TransitionSimToRendering(EI_CommandContext& context);
    void TransitionRenderingToSim(EI_CommandContext& context);
    void UpdateBones(EI_CommandContext& context);

    // A1 threading split:
    // MAIN thread: snapshot the skeleton's skinning matrices (pose x rest^-1)
    // as raw AMD::float4x4 bytes. Safe: touches Skeleton3D only.
    godot::PackedByteArray SnapshotBoneMatrices() const;
    // RENDER thread: write a snapshot into the hair object's constant buffer.
    void ApplyBoneMatricesBytes(const godot::PackedByteArray& bytes);

    // RID of the simulated positions buffer (for async readback). Invalid RID
    // until EnsureTressFXObjectCreated() has run.
    godot::RID GetPositionsBufferRID() const;

    int GetVertsPerStrand() const;
    int GetFollowPerGuide() const;

    // CPU-only: slice guide-strand positions out of a full positions-buffer
    // readback (interleaved guide+follow slots), same output layout as
    // PackGuidePositionsVec4. Used by the async debug-line path.
    bool ExtractGuidePositionsVec4FromBytes(const godot::PackedByteArray& all_positions,
        int guide_strand_limit,
        godot::PackedByteArray& out_bytes, int& out_vertices_per_strand, int& out_guide_strands) const;

    // Path A (CPU-only bring-up): build a simple line mesh from the loaded asset positions.
    // Coordinates are in the hair asset's local space (attach the MeshInstance as a child of the character).
    // Returns null if the asset isn't loaded.
    godot::Ref<godot::ArrayMesh> CreateDebugLineMesh(bool guides_only, int strand_limit) const;

    // Path A (CPU-only): build a line mesh where each strand is CPU-skinned using the .tfxbone weights.
    // Requires a valid EI_Scene with a Skeleton3D set.
    godot::Ref<godot::ArrayMesh> CreateDebugSkinnedLineMesh(bool guides_only, int strand_limit) const;

    int GetGuideStrandCount() const;
    int GetTotalStrandCount() const;

    // Packs guide strand vertex positions into a tightly-packed std430-friendly buffer.
    // Layout: vec4 position (xyz used, w=1). Count = guide_strands * vertices_per_strand.
    // Returns false if the asset isn't loaded.
    bool PackGuidePositionsVec4(godot::PackedByteArray& out_bytes, int& out_vertices_per_strand, int& out_guide_strands) const;

    // Like PackGuidePositionsVec4, but sources positions from the simulated GPU buffer.
    // This is a bring-up/debug path: it performs a local-RD GPU sync + CPU readback.
    bool PackSimulatedGuidePositionsVec4(godot::PackedByteArray& out_bytes, int& out_vertices_per_strand, int& out_guide_strands, int guide_strand_limit) const;

private:
    std::unique_ptr<TressFXHairObject> m_pStrands;
    std::unique_ptr<TressFXAsset> m_asset;
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
