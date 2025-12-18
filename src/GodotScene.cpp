#include "GodotScene.h"

#include <godot_cpp/classes/skeleton3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

EI_Scene::EI_Scene() = default;
EI_Scene::~EI_Scene() = default;

void EI_Scene::set_skeleton(godot::Skeleton3D* skeleton) {
    m_skeleton = skeleton;
    m_cached_skeleton_version = 0;
    m_cached_world_mats.clear();

    if (!m_skeleton) {
        godot::UtilityFunctions::print("EI_Scene: skeleton cleared");
        return;
    }

    godot::UtilityFunctions::print(
        godot::String("EI_Scene: skeleton set bone_count=") + godot::String::num_int64(m_skeleton->get_bone_count()));
}

static XMMATRIX transform3d_to_xmmatrix(const godot::Transform3D& t) {
    // Godot uses Basis + origin. We pack into a 4x4 matrix.
    // Convention: this is primarily consumed by TressFXBoneSkinning's math helpers.
    const godot::Basis& b = t.basis;
    const godot::Vector3& o = t.origin;

    XMMATRIX m;

    // Our XMMATRIX + XMVector4Transform implementation assumes row-vector multiplication:
    //   v' = v * M
    // with translation stored in the last row (m[3][0..2]).
    // Godot's Basis columns are the local axes in parent space, so they map to matrix columns.
    const godot::Vector3 x = b.get_column(0);
    const godot::Vector3 y = b.get_column(1);
    const godot::Vector3 z = b.get_column(2);

    // Column 0 (X axis)
    m.m[0][0] = x.x;
    m.m[1][0] = x.y;
    m.m[2][0] = x.z;
    m.m[3][0] = o.x;

    // Column 1 (Y axis)
    m.m[0][1] = y.x;
    m.m[1][1] = y.y;
    m.m[2][1] = y.z;
    m.m[3][1] = o.y;

    // Column 2 (Z axis)
    m.m[0][2] = z.x;
    m.m[1][2] = z.y;
    m.m[2][2] = z.z;
    m.m[3][2] = o.z;

    // Column 3
    m.m[0][3] = 0.0f;
    m.m[1][3] = 0.0f;
    m.m[2][3] = 0.0f;
    m.m[3][3] = 1.0f;

    return m;
}

int EI_Scene::GetBoneIdByName(int /*skinNumber*/, const char* name) {
    if (!m_skeleton || !name || name[0] == '\0') {
        if (!m_skeleton) {
            godot::UtilityFunctions::push_warning("EI_Scene: GetBoneIdByName called with no skeleton set");
        }
        return 0;
    }

    const godot::String bone_name(name);
    const int32_t idx = m_skeleton->find_bone(bone_name);
    if (idx < 0) {
        godot::UtilityFunctions::push_warning(godot::String("EI_Scene: bone not found: '") + bone_name + godot::String("'"));
        return 0;
    }
    return idx;
}

std::vector<XMMATRIX>& EI_Scene::GetWorldSpaceSkeletonMats(int /*skinNumber*/) {
    if (!m_skeleton) {
        m_cached_world_mats.clear();
        m_cached_skeleton_version = 0;
        return m_cached_world_mats;
    }

    const uint64_t version = m_skeleton->get_version();
    if (version == m_cached_skeleton_version && !m_cached_world_mats.empty()) {
        return m_cached_world_mats;
    }

    const int32_t bone_count = m_skeleton->get_bone_count();
    m_cached_world_mats.resize(bone_count);

    for (int32_t i = 0; i < bone_count; ++i) {
        // Return skinning matrices: current_pose * inverse(rest_pose).
        // This matches the convention used by the original TressFX glue and gives
        // stable deformation for our current math packing.
        const godot::Transform3D pose = m_skeleton->get_bone_global_pose_no_override(i);
        const godot::Transform3D rest = m_skeleton->get_bone_global_rest(i);
        const godot::Transform3D skin = pose * rest.affine_inverse();
        m_cached_world_mats[i] = transform3d_to_xmmatrix(skin);
    }

    m_cached_skeleton_version = version;
    return m_cached_world_mats;
}

static AMD::float4x4 identity_4x4() {
    AMD::float4x4 identity;
    std::memset(&identity, 0, sizeof(identity));
    identity.m[0] = 1.0f;
    identity.m[5] = 1.0f;
    identity.m[10] = 1.0f;
    identity.m[15] = 1.0f;
    return identity;
}

AMD::float4x4 EI_Scene::GetMV() {
    return identity_4x4();
}

AMD::float4x4 EI_Scene::GetMVP() {
    return identity_4x4();
}
