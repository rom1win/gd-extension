#pragma once

// Phase B3.2: C++ port of tools/bind_hair_prototype.py (Gate B1 answer-key
// experiment, top1=100%/meanL1=0.0001 on Ratboy). Binds hair-strand root
// positions to a triangle mesh's per-vertex skin weights: nearest-triangle
// (closest-point-on-triangle, Ericson 5.1.5) + barycentric blend of the
// touching triangle's 3 corners' bone weights, aggregated by bone index,
// top-4 kept, renormalized to sum 1. Pure CPU math, no Godot/GPU types in
// the core -- keep it that way so it stays unit-testable against the Python
// prototype's numbers.

#include <vector>
#include <array>
#include <string>

#include "TressFX/TressFXAsset.h" // Vector3, TressFXBoneSkinningData

namespace godot {
class MeshInstance3D;
class Skeleton3D;
}

namespace HairBinding {

// B3.2 diagnostic-only (SUSPICIOUS bind-check hunt, 2026-07-20): snapshot of
// the mesh/skeleton identity data BindRootsToGodotMesh resolved, so a caller
// can print it without re-deriving it. Does not affect binding math.
struct MeshDiagnosticInfo {
    int skinBindCount = -1; // -1 = no Skin resource found
    // First up-to-3 Skin::get_bind_name() values, or "EMPTY" per-entry if
    // that bind's name was the empty StringName (a common, harmless glTF
    // import pattern IF get_bind_bone() is populated instead -- see the
    // fallback in BindRootsToGodotMesh's .cpp).
    std::array<std::string, 3> firstBindNames = { "?", "?", "?" };
    std::array<std::string, 3> firstSkeletonBoneNames = { "?", "?", "?" };
    // Mesh-wide: how many of the mesh's vertices ended up with AT LEAST ONE
    // resolved (index>=0, weight>0) bone influence after the Skin bind ->
    // skeleton-index mapping, out of vertexCount total. resolvedWeightVerts
    // near 0 pins the failure to the Skin/bone-identity mapping; near
    // vertexCount means mapping is fine and any mismatch is elsewhere
    // (e.g. the mesh-to-skeleton-space transform).
    int vertexCount = 0;
    int resolvedWeightVerts = 0;
};

// Core narrow/broad-phase math. Engine-agnostic: every array is plain data,
// `boneIndices4`/`boneWeights4` are copied straight into the output in
// whatever index space the caller already resolved them to (skeleton bone
// indices, mesh-local indices, anything) -- this function never interprets
// bone identity, only geometry.
//
//   rootPositions   - one entry per hair root to bind.
//   meshVerts       - triangle mesh vertex positions, SAME SPACE as rootPositions.
//   meshNormals     - optional (may be null / empty); not used by the current
//                     nearest-triangle algorithm, kept for signature parity
//                     with a future normal-aware projection.
//   boneIndices4/boneWeights4 - per meshVerts[i], up to 4 (index,weight)
//                     influences; unused slots marked with index < 0 or
//                     weight <= 0.
//   triangles       - vertex-index triples into meshVerts.
//   outSkinningData - resized to rootPositions.size(); outSkinningData[i]
//                     is the top-4, weight-renormalized bone blend for
//                     rootPositions[i]. Falls back to 100% bone 0 if the
//                     mesh has no usable triangles/weights at all.
//   outClosestPoints/outDistances - optional (may be null); if non-null,
//                     resized to rootPositions.size() and filled with the
//                     found closest surface point / distance to it (distance
//                     -1 if no candidate triangle was found for that root,
//                     which is when the bone-0 fallback fires). Diagnostic
//                     only, zero cost when null.
void BindRootsToMesh(
    const std::vector<Vector3>& rootPositions,
    const std::vector<Vector3>& meshVerts,
    const std::vector<Vector3>* meshNormals,
    const std::vector<std::array<int, 4>>& boneIndices4,
    const std::vector<std::array<float, 4>>& boneWeights4,
    const std::vector<std::array<int, 3>>& triangles,
    std::vector<TressFXBoneSkinningData>& outSkinningData,
    std::vector<Vector3>* outClosestPoints = nullptr,
    std::vector<float>* outDistances = nullptr);

// Godot-facing wrapper: pulls surface-0 ARRAY_VERTEX/ARRAY_BONES/
// ARRAY_WEIGHTS/ARRAY_INDEX off `body_mesh`'s Mesh resource, maps mesh-local
// (Skin bind-index) bone indices to `skeleton` bone indices via the mesh's
// Skin resource (bind name -> Skeleton3D::find_bone), transforms mesh
// vertices from the MeshInstance3D's local space into the SAME space
// `rootPositionsSkeletonSpace` already lives in (skeleton model space --
// see the .cpp for why), and calls BindRootsToMesh.
//
// MAIN THREAD ONLY: touches MeshInstance3D/Skeleton3D/Skin. Call at asset
// load time, never from render-thread code.
//
// Returns false (outSkinningData left untouched) if body_mesh/skeleton are
// null, the mesh has no surfaces, or it has no Skin resource to resolve
// bone identity through -- callers should fall back to uniform/rigid
// skinning in that case.
//
// outClosestPoints/outDistances: see BindRootsToMesh. outMeshDiag: optional
// (may be null); if non-null, filled with the resolved Skin/skeleton
// identity snapshot described above. All three are diagnostic-only.
bool BindRootsToGodotMesh(
    godot::MeshInstance3D* body_mesh,
    godot::Skeleton3D* skeleton,
    const std::vector<Vector3>& rootPositionsSkeletonSpace,
    std::vector<TressFXBoneSkinningData>& outSkinningData,
    std::vector<Vector3>* outClosestPoints = nullptr,
    std::vector<float>* outDistances = nullptr,
    MeshDiagnosticInfo* outMeshDiag = nullptr);

} // namespace HairBinding
