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

    // Convention: row-major XMMATRIX, each Godot basis column placed as a matrix ROW.
    //
    // CPU path (TressFXBoneSkinning.cpp): XMVector4Transform(v, M) = v * M
    //   result.x = v.x*M[0][0] + v.y*M[1][0] + v.z*M[2][0] + 1*M[3][0]
    //   For this to equal Basis*v+o: M[0][0]=x.x, M[1][0]=y.x, M[2][0]=z.x, M[3][0]=o.x
    //   → each basis column (x,y,z) goes into a matrix ROW (not a column).
    //
    // GPU path (GLSL simulation shaders): mat4 reads same memory as column-major.
    //   GLSL col0 = memory[0..3] = (x.x,x.y,x.z,0)  [row 0 re-read as col 0]
    //   (M * vec4(v,1)).x = col0[0]*v.x + col1[0]*v.y + col2[0]*v.z + col3[0]
    //                     = x.x*v.x + y.x*v.y + z.x*v.z + o.x  ✓
    const godot::Vector3 x = b.get_column(0);
    const godot::Vector3 y = b.get_column(1);
    const godot::Vector3 z = b.get_column(2);

    // Row 0: x-basis column as matrix row
    m.m[0][0] = x.x; m.m[0][1] = x.y; m.m[0][2] = x.z; m.m[0][3] = 0.0f;
    // Row 1: y-basis column as matrix row
    m.m[1][0] = y.x; m.m[1][1] = y.y; m.m[1][2] = y.z; m.m[1][3] = 0.0f;
    // Row 2: z-basis column as matrix row
    m.m[2][0] = z.x; m.m[2][1] = z.y; m.m[2][2] = z.z; m.m[2][3] = 0.0f;
    // Row 3: origin (translation)
    m.m[3][0] = o.x; m.m[3][1] = o.y; m.m[3][2] = o.z; m.m[3][3] = 1.0f;

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
