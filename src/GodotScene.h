#pragma once

#include "GodotTressFXMath.h"
#include <cstdint>
#include <vector>
#include <string>

// Replacement for SceneGLTFImpl.h / EI_Scene
// Implements the interface required by TressFXBoneSkinning and TressFXSDFMarchingCubes

namespace godot {
class Skeleton3D;
}

class EI_Scene {
public:
    EI_Scene();
    virtual ~EI_Scene();

    void set_skeleton(godot::Skeleton3D* skeleton);
    godot::Skeleton3D* get_skeleton() const { return m_skeleton; }

    // Required by TressFXBoneSkinning.cpp
    int GetBoneIdByName(int skinNumber, const char* name);
    std::vector<XMMATRIX>& GetWorldSpaceSkeletonMats(int skinNumber);

    // Used by TressFXBoneSkinning for debug shading.
    AMD::float4x4 GetMV();
    AMD::float4x4 GetMVP();

private:
    godot::Skeleton3D* m_skeleton = nullptr;
    std::vector<XMMATRIX> m_cached_world_mats;
    uint64_t m_cached_skeleton_version = 0;
};
