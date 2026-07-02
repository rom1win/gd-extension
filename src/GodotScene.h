#pragma once

#include "GodotTressFXMath.h"
#include <cstdint>
#include <vector>
#include <string>

// Replacement for SceneGLTFImpl.h / EI_Scene
// Provides bone lookup/matrices for TressFXAsset::LoadBoneData and our skinning paths.

namespace godot {
class Skeleton3D;
}

class EI_Scene {
public:
    EI_Scene();
    virtual ~EI_Scene();

    void set_skeleton(godot::Skeleton3D* skeleton);
    godot::Skeleton3D* get_skeleton() const { return m_skeleton; }

    // Required by TressFXAsset::LoadBoneData.
    int GetBoneIdByName(int skinNumber, const char* name);
    // Used by our CPU/GPU skinning paths (HairStrands).
    std::vector<XMMATRIX>& GetWorldSpaceSkeletonMats(int skinNumber);

private:
    godot::Skeleton3D* m_skeleton = nullptr;
    std::vector<XMMATRIX> m_cached_world_mats;
    uint64_t m_cached_skeleton_version = 0;
};
