#include "HairBinding.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/classes/skin.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace {

// --- closest point on triangle (Ericson, "Real-Time Collision Detection"
// 5.1.5) -- direct port of bind_hair_prototype.py's closest_point_on_triangle,
// same branch structure, same variable names where practical so the two stay
// comparable line-for-line. ---
Vector3 ClosestPointOnTriangle(const Vector3& p, const Vector3& a, const Vector3& b, const Vector3& c,
    float& outU, float& outV, float& outW) {
    const Vector3 ab = b - a;
    const Vector3 ac = c - a;
    const Vector3 ap = p - a;
    const float d1 = ab.Dot(ap);
    const float d2 = ac.Dot(ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        outU = 1.0f; outV = 0.0f; outW = 0.0f;
        return a;
    }

    const Vector3 bp = p - b;
    const float d3 = ab.Dot(bp);
    const float d4 = ac.Dot(bp);
    if (d3 >= 0.0f && d4 <= d3) {
        outU = 0.0f; outV = 1.0f; outW = 0.0f;
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float v = d1 / (d1 - d3);
        outU = 1.0f - v; outV = v; outW = 0.0f;
        return a + ab * v;
    }

    const Vector3 cp = p - c;
    const float d5 = ab.Dot(cp);
    const float d6 = ac.Dot(cp);
    if (d6 >= 0.0f && d5 <= d6) {
        outU = 0.0f; outV = 0.0f; outW = 1.0f;
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float w = d2 / (d2 - d6);
        outU = 1.0f - w; outV = 0.0f; outW = w;
        return a + ac * w;
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        outU = 0.0f; outV = 1.0f - w; outW = w;
        return b + (c - b) * w;
    }

    const float denom = 1.0f / (va + vb + vc);
    const float v = vb * denom;
    const float w = vc * denom;
    outU = 1.0f - v - w; outV = v; outW = w;
    return a + ab * v + ac * w;
}

struct GridKey {
    int x, y, z;
    bool operator==(const GridKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct GridKeyHash {
    size_t operator()(const GridKey& k) const {
        size_t h = std::hash<int>()(k.x);
        h ^= std::hash<int>()(k.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

} // namespace

void HairBinding::BindRootsToMesh(
    const std::vector<Vector3>& rootPositions,
    const std::vector<Vector3>& meshVerts,
    const std::vector<Vector3>* /*meshNormals*/,
    const std::vector<std::array<int, 4>>& boneIndices4,
    const std::vector<std::array<float, 4>>& boneWeights4,
    const std::vector<std::array<int, 3>>& triangles,
    std::vector<TressFXBoneSkinningData>& outSkinningData,
    std::vector<Vector3>* outClosestPoints,
    std::vector<float>* outDistances) {

    TressFXBoneSkinningData fallback{};
    fallback.boneIndex[0] = 0.0f; fallback.boneIndex[1] = 0.0f;
    fallback.boneIndex[2] = 0.0f; fallback.boneIndex[3] = 0.0f;
    fallback.weight[0] = 1.0f; fallback.weight[1] = 0.0f;
    fallback.weight[2] = 0.0f; fallback.weight[3] = 0.0f;

    outSkinningData.assign(rootPositions.size(), fallback);
    if (outClosestPoints) outClosestPoints->assign(rootPositions.size(), Vector3());
    if (outDistances) outDistances->assign(rootPositions.size(), -1.0f);

    const size_t numVerts = meshVerts.size();
    const size_t numTris = triangles.size();
    if (numVerts == 0 || numTris == 0 || boneIndices4.size() != numVerts || boneWeights4.size() != numVerts) {
        return;
    }

    // vertex -> touching triangle indices (matches the Python prototype's
    // vertex_to_tris).
    std::vector<std::vector<int>> vertexToTris(numVerts);
    for (size_t ti = 0; ti < numTris; ++ti) {
        const auto& t = triangles[ti];
        for (int k = 0; k < 3; ++k) {
            if (t[k] >= 0 && (size_t)t[k] < numVerts) {
                vertexToTris[t[k]].push_back((int)ti);
            }
        }
    }

    // Broad phase: uniform spatial hash over mesh vertices (stands in for the
    // prototype's KNN -- same purpose, no numpy/O(N) sort needed). Cell size
    // targets roughly one vertex per cell on average.
    Vector3 mn = meshVerts[0], mx = meshVerts[0];
    for (const auto& v : meshVerts) {
        mn.x = std::min(mn.x, v.x); mn.y = std::min(mn.y, v.y); mn.z = std::min(mn.z, v.z);
        mx.x = std::max(mx.x, v.x); mx.y = std::max(mx.y, v.y); mx.z = std::max(mx.z, v.z);
    }
    const Vector3 extent = mx - mn;
    const double volume = std::max(1e-9, (double)extent.x * (double)extent.y * (double)extent.z);
    float cellSize = (float)std::cbrt(volume / (double)numVerts);
    if (!(cellSize > 1e-6f)) {
        const float diag = std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z);
        cellSize = std::max(diag / 32.0f, 1e-4f);
    }

    auto cellOf = [&](const Vector3& p) -> GridKey {
        return GridKey{
            (int)std::floor((p.x - mn.x) / cellSize),
            (int)std::floor((p.y - mn.y) / cellSize),
            (int)std::floor((p.z - mn.z) / cellSize) };
    };

    std::unordered_map<GridKey, std::vector<int>, GridKeyHash> grid;
    for (size_t vi = 0; vi < numVerts; ++vi) {
        grid[cellOf(meshVerts[vi])].push_back((int)vi);
    }

    const int kMinCandidateTris = 8;
    const int kMaxRing = 8;

    for (size_t ri = 0; ri < rootPositions.size(); ++ri) {
        const Vector3& root = rootPositions[ri];
        const GridKey c0 = cellOf(root);

        std::unordered_set<int> triSet;
        auto gatherRing = [&](int ring) {
            triSet.clear();
            for (int dz = -ring; dz <= ring; ++dz) {
                for (int dy = -ring; dy <= ring; ++dy) {
                    for (int dx = -ring; dx <= ring; ++dx) {
                        const auto it = grid.find(GridKey{ c0.x + dx, c0.y + dy, c0.z + dz });
                        if (it == grid.end()) continue;
                        for (int vi : it->second) {
                            for (int ti : vertexToTris[vi]) {
                                triSet.insert(ti);
                            }
                        }
                    }
                }
            }
        };
        int satisfiedRing = -1;
        for (int ring = 0; ring <= kMaxRing; ++ring) {
            gatherRing(ring);
            if ((int)triSet.size() >= kMinCandidateTris) {
                satisfiedRing = ring;
                break;
            }
        }
        // Safety margin: expand one more ring past the first that satisfied
        // kMinCandidateTris (mirrors the Python prototype's generosity --
        // K=40 nearest vertices there vs. our per-cell threshold here --
        // cheap at this problem size and avoids missing the true closest
        // triangle when it sits just across a cell boundary).
        if (satisfiedRing >= 0 && satisfiedRing < kMaxRing) {
            gatherRing(satisfiedRing + 1);
        }
        if (triSet.empty()) {
            // Degenerate/very sparse mesh: fall back to full brute force
            // rather than leaving this root unbound.
            for (size_t ti = 0; ti < numTris; ++ti) triSet.insert((int)ti);
        }

        float bestD2 = std::numeric_limits<float>::infinity();
        int bestTri = -1;
        float bestU = 0.0f, bestV = 0.0f, bestW = 0.0f;
        for (int ti : triSet) {
            const auto& t = triangles[ti];
            float u, v, w;
            const Vector3 cp = ClosestPointOnTriangle(root, meshVerts[t[0]], meshVerts[t[1]], meshVerts[t[2]], u, v, w);
            const Vector3 d = cp - root;
            const float d2 = d.Dot(d);
            if (d2 < bestD2) {
                bestD2 = d2; bestTri = ti; bestU = u; bestV = v; bestW = w;
            }
        }
        if (bestTri < 0) {
            continue; // leave the fallback (bone 0, weight 1) already assigned above
        }

        if (outClosestPoints || outDistances) {
            const auto& bt = triangles[bestTri];
            float u2, v2, w2;
            const Vector3 cp = ClosestPointOnTriangle(root, meshVerts[bt[0]], meshVerts[bt[1]], meshVerts[bt[2]], u2, v2, w2);
            if (outClosestPoints) (*outClosestPoints)[ri] = cp;
            if (outDistances) (*outDistances)[ri] = std::sqrt(bestD2);
        }

        // Blend the touching triangle's 3 corners' skin weights by barycentric
        // coordinate, aggregated by bone index (bind_hair_prototype.py's
        // `blended` dict, keyed by name there -- here the caller already
        // resolved names to indices, so we key by index directly).
        const auto& tri = triangles[bestTri];
        const float bary[3] = { bestU, bestV, bestW };
        std::unordered_map<int, float> blended;
        for (int corner = 0; corner < 3; ++corner) {
            const int vidx = tri[corner];
            const float baryCoord = bary[corner];
            for (int k = 0; k < 4; ++k) {
                const int bidx = boneIndices4[vidx][k];
                const float w = boneWeights4[vidx][k];
                if (bidx < 0 || w <= 0.0f) continue;
                blended[bidx] += baryCoord * w;
            }
        }
        if (blended.empty()) {
            continue; // leave the fallback
        }

        std::vector<std::pair<int, float>> items(blended.begin(), blended.end());
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (items.size() > 4) items.resize(4);

        float total = 0.0f;
        for (const auto& it : items) total += it.second;
        if (total <= 0.0f) {
            continue; // leave the fallback
        }

        TressFXBoneSkinningData skin{};
        for (size_t k = 0; k < 4; ++k) {
            if (k < items.size()) {
                skin.boneIndex[k] = (float)items[k].first;
                skin.weight[k] = items[k].second / total;
            } else {
                skin.boneIndex[k] = 0.0f;
                skin.weight[k] = 0.0f;
            }
        }
        outSkinningData[ri] = skin;
    }
}

namespace {

// Godot's standard skinned-mesh convention (glTF/Blender import, matches how
// EI_Scene::GetWorldSpaceSkeletonMats treats Skeleton3D::get_bone_global_pose/
// get_bone_global_rest as "skeleton model space", i.e. relative to the
// Skeleton3D node, NOT world space): the Skin resource's bind poses are
// authored against the skeleton's REST pose, so a mesh's raw ARRAY_VERTEX
// (undeformed, pre-skin, in the MeshInstance3D's own local frame) already
// equals its skinned position at rest -- no per-vertex un-skinning needed,
// only the static node-to-node transform. This is the same assumption every
// other skinned-mesh consumer in this codebase makes (GodotScene.cpp's
// pose*rest^-1 skinning matrices, SDF.cpp's CollisionMesh) and is standard
// for glTF/Blender-exported characters (RatBoy included); it would only be
// wrong for a mesh deliberately bound in a non-rest pose, which none of our
// assets are.
godot::Transform3D MeshLocalToSkeletonModelSpace(godot::MeshInstance3D* body_mesh, godot::Skeleton3D* skeleton) {
    return skeleton->get_global_transform().affine_inverse() * body_mesh->get_global_transform();
}

} // namespace

bool HairBinding::BindRootsToGodotMesh(
    godot::MeshInstance3D* body_mesh,
    godot::Skeleton3D* skeleton,
    const std::vector<Vector3>& rootPositionsSkeletonSpace,
    std::vector<TressFXBoneSkinningData>& outSkinningData,
    std::vector<Vector3>* outClosestPoints,
    std::vector<float>* outDistances,
    MeshDiagnosticInfo* outMeshDiag) {
    // Deliberately NOT `using namespace godot;` here: this file constructs the
    // global (AMD) ::Vector3 by its bare name throughout -- a using-directive
    // would make that ambiguous against godot::Vector3.

    if (!body_mesh || !skeleton) {
        return false;
    }

    godot::Ref<godot::Mesh> mesh = body_mesh->get_mesh();
    if (mesh.is_null() || mesh->get_surface_count() <= 0) {
        godot::UtilityFunctions::push_warning("HairBinding: bind_body_path mesh has no surfaces");
        return false;
    }

    const godot::Array arrays = mesh->surface_get_arrays(0);
    if (arrays.size() <= godot::Mesh::ARRAY_INDEX) {
        godot::UtilityFunctions::push_warning("HairBinding: bind_body_path mesh surface 0 has no array data");
        return false;
    }

    const godot::PackedVector3Array verts = arrays[godot::Mesh::ARRAY_VERTEX];
    const godot::PackedInt32Array indices = arrays[godot::Mesh::ARRAY_INDEX];
    const godot::Variant bones_v = arrays[godot::Mesh::ARRAY_BONES];
    const godot::Variant weights_v = arrays[godot::Mesh::ARRAY_WEIGHTS];
    if (verts.size() == 0 || bones_v.get_type() != godot::Variant::PACKED_INT32_ARRAY ||
        weights_v.get_type() != godot::Variant::PACKED_FLOAT32_ARRAY) {
        godot::UtilityFunctions::push_warning("HairBinding: bind_body_path mesh has no bone weights (not a skinned mesh?)");
        return false;
    }
    const godot::PackedInt32Array bones = bones_v;
    const godot::PackedFloat32Array weights = weights_v;

    const int64_t vcount = verts.size();
    const int64_t influences = (vcount > 0) ? (bones.size() / vcount) : 0;
    if (influences != 4 && influences != 8) {
        godot::UtilityFunctions::push_warning(godot::String("HairBinding: unexpected bone-influence width ") +
            godot::String::num_int64(influences) + godot::String(" (expected 4 or 8)"));
        return false;
    }
    if (influences == 8) {
        static bool warned_eight = false;
        if (!warned_eight) {
            warned_eight = true;
            godot::UtilityFunctions::push_warning("HairBinding: bind_body_path mesh uses 8-bone weights; using first 4 influences (renormalized)");
        }
    }

    // Mesh-local (Skin bind-index) bone index -> Skeleton3D bone index, by name.
    godot::Ref<godot::Skin> skin = body_mesh->get_skin();
    if (skin.is_null()) {
        godot::UtilityFunctions::push_warning("HairBinding: bind_body_path mesh has no Skin resource; cannot resolve bone identity");
        return false;
    }
    const int32_t bindCount = skin->get_bind_count();
    std::vector<int> bindToSkeleton(bindCount, -1);
    for (int32_t i = 0; i < bindCount; ++i) {
        const godot::StringName bindName = skin->get_bind_name(i);
        int32_t skelIdx = -1;
        if (bindName != godot::StringName()) {
            skelIdx = skeleton->find_bone(godot::String(bindName));
        }
        if (skelIdx < 0) {
            // Some import paths only fill bind_bone (already a skeleton index).
            skelIdx = skin->get_bind_bone(i);
        }
        bindToSkeleton[i] = skelIdx;
    }

    if (outMeshDiag) {
        outMeshDiag->skinBindCount = bindCount;
        const int32_t skelBoneCount = skeleton->get_bone_count();
        for (int i = 0; i < 3; ++i) {
            if (i < bindCount) {
                const godot::StringName bn = skin->get_bind_name(i);
                outMeshDiag->firstBindNames[i] = (bn == godot::StringName())
                    ? std::string("EMPTY(bind_bone=") + std::to_string(skin->get_bind_bone(i)) + ")"
                    : std::string(godot::String(bn).utf8().get_data());
            } else {
                outMeshDiag->firstBindNames[i] = "(n/a)";
            }
            if (i < skelBoneCount) {
                outMeshDiag->firstSkeletonBoneNames[i] = std::string(godot::String(skeleton->get_bone_name(i)).utf8().get_data());
            } else {
                outMeshDiag->firstSkeletonBoneNames[i] = "(n/a)";
            }
        }
    }

    const godot::Transform3D toSkeletonSpace = MeshLocalToSkeletonModelSpace(body_mesh, skeleton);

    std::vector<Vector3> meshVerts(vcount);
    std::vector<std::array<int, 4>> boneIndices4(vcount);
    std::vector<std::array<float, 4>> boneWeights4(vcount);
    int resolvedWeightVerts = 0;
    for (int64_t vi = 0; vi < vcount; ++vi) {
        const godot::Vector3 gp = toSkeletonSpace.xform(verts[vi]);
        meshVerts[vi] = Vector3(gp.x, gp.y, gp.z);

        bool anyResolved = false;
        for (int k = 0; k < 4; ++k) {
            const int64_t bindIdx = bones[vi * influences + k];
            const float w = weights[vi * influences + k];
            int skelIdx = -1;
            if (w > 0.0f && bindIdx >= 0 && bindIdx < bindCount) {
                skelIdx = bindToSkeleton[bindIdx];
            }
            boneIndices4[vi][k] = skelIdx;
            boneWeights4[vi][k] = (skelIdx >= 0) ? w : 0.0f;
            if (skelIdx >= 0) anyResolved = true;
        }
        if (anyResolved) ++resolvedWeightVerts;
    }
    if (outMeshDiag) {
        outMeshDiag->vertexCount = (int)vcount;
        outMeshDiag->resolvedWeightVerts = resolvedWeightVerts;
    }

    std::vector<std::array<int, 3>> triangles;
    if (indices.size() >= 3) {
        triangles.resize(indices.size() / 3);
        for (size_t ti = 0; ti < triangles.size(); ++ti) {
            triangles[ti] = { indices[(int)ti * 3 + 0], indices[(int)ti * 3 + 1], indices[(int)ti * 3 + 2] };
        }
    } else if (vcount >= 3) {
        // Non-indexed surface: vertices are already grouped in triangle triples.
        triangles.resize((size_t)vcount / 3);
        for (size_t ti = 0; ti < triangles.size(); ++ti) {
            triangles[ti] = { (int)ti * 3 + 0, (int)ti * 3 + 1, (int)ti * 3 + 2 };
        }
    } else {
        godot::UtilityFunctions::push_warning("HairBinding: bind_body_path mesh has no usable triangles");
        return false;
    }

    BindRootsToMesh(rootPositionsSkeletonSpace, meshVerts, nullptr, boneIndices4, boneWeights4, triangles,
        outSkinningData, outClosestPoints, outDistances);
    return true;
}
