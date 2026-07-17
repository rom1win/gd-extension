#version 450

// A3.2 subtask 2: skins the SDF collision mesh to the current skeleton pose.
// Faithful port of thirdparty/tressfx/src/Shaders/TressFXBoneSkinning.hlsl,
// entry point `BoneSkinning` (lines 57-94): one thread per collision-mesh
// vertex, up to 4 weighted bone matrices applied to both position and normal.
//
// Bindings: own private descriptor set (set 0), NOT shared with the six
// TressFXSimulation.*.comp.glsl kernels (which bind their own sets 0/1) or
// with any other kernel's descriptor set -- a compute pipeline's bind-group
// layout is private to that pipeline, so binding-number reuse across
// different shaders is normal and cannot collide. Numbers below mirror AMD's
// original HLSL `[[vk::binding(n, 0)]]` annotations for this same shader
// (u0/t1/t2/b3), which is also the order CreateBoneSkinningLayout() in
// TressFXLayouts.cpp already declares (still compiled into this build, reused
// by src/SDF.cpp for this kernel's PSO):
//   binding 0 (RW)      bs_collMeshVertexPositions  -- skinned output (UAV)
//   binding 1 (RO)      bs_boneSkinningData         -- per-vertex bone idx/weight
//   binding 2 (RO)      bs_initialVertexPositions   -- rest pose (pos+normal)
//   binding 3 (uniform) ConstBufferCS_BoneMatrix     -- bone matrices + vertex count

#define THREAD_GROUP_SIZE 64
// Matches kBoneCount in src/tressfx_character.cpp and the AMD_TRESSFX_MAX_NUM_BONES
// used by TressFXSimulation.*.comp.glsl -- our skeletons stay well under this cap.
#define AMD_TRESSFX_MAX_NUM_BONES 128

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

struct StandardVertex {
    vec4 position;
    vec4 normal;
};

struct BoneSkinningData {
    vec4 boneIndex;
    vec4 boneWeight;
};

layout(set = 0, binding = 0, std430) buffer OutCollMeshVertexPositions {
    StandardVertex bs_collMeshVertexPositions[];
};

layout(set = 0, binding = 1, std430) readonly buffer InBoneSkinningData {
    BoneSkinningData bs_boneSkinningData[];
};

layout(set = 0, binding = 2, std430) readonly buffer InInitialVertexPositions {
    StandardVertex bs_initialVertexPositions[];
};

layout(set = 0, binding = 3, std140) uniform ConstBufferCS_BoneMatrix {
    ivec4 g_NumMeshVertices;
    // Same packing convention as TressFXSimulation.*.comp.glsl's
    // g_BoneSkinningMatrix (see src/GodotScene.cpp for the authoritative
    // comment): the CPU packs a row-major skinning matrix; read here as
    // GLSL's default column-major, multiplying as (M * v) reproduces the
    // HLSL mul(v, row_major_M) result.
    mat4 g_BoneSkinningMatrix[AMD_TRESSFX_MAX_NUM_BONES];
} cb;

void main() {
    uint global_id = gl_GlobalInvocationID.x;
    if (global_id >= uint(cb.g_NumMeshVertices.x)) {
        return;
    }

    vec3 pos = bs_initialVertexPositions[global_id].position.xyz;
    vec3 n = bs_initialVertexPositions[global_id].normal.xyz;

    // compute a bone skinning transform
    BoneSkinningData skinning = bs_boneSkinningData[global_id];

    int idx0 = clamp(int(skinning.boneIndex.x), 0, AMD_TRESSFX_MAX_NUM_BONES - 1);
    int idx1 = clamp(int(skinning.boneIndex.y), 0, AMD_TRESSFX_MAX_NUM_BONES - 1);
    int idx2 = clamp(int(skinning.boneIndex.z), 0, AMD_TRESSFX_MAX_NUM_BONES - 1);
    int idx3 = clamp(int(skinning.boneIndex.w), 0, AMD_TRESSFX_MAX_NUM_BONES - 1);

    // Interpolate world space bone matrices using weights.
    mat4 bone_matrix = cb.g_BoneSkinningMatrix[idx0] * skinning.boneWeight.x;
    float weight_sum = skinning.boneWeight.x;

    // Each vertex gets influence from four bones. In case there are less than four bones,
    // boneIndex and boneWeight would be zero. This number four was set in the Maya exporter
    // and also used in the loader; do not change without updating every consumer.
    if (skinning.boneWeight.y > 0.0) { bone_matrix += cb.g_BoneSkinningMatrix[idx1] * skinning.boneWeight.y; weight_sum += skinning.boneWeight.y; }
    if (skinning.boneWeight.z > 0.0) { bone_matrix += cb.g_BoneSkinningMatrix[idx2] * skinning.boneWeight.z; weight_sum += skinning.boneWeight.z; }
    if (skinning.boneWeight.w > 0.0) { bone_matrix += cb.g_BoneSkinningMatrix[idx3] * skinning.boneWeight.w; weight_sum += skinning.boneWeight.w; }

    if (weight_sum > 1e-6) {
        bone_matrix /= weight_sum;
    }

    pos = (bone_matrix * vec4(pos, 1.0)).xyz;
    n = (bone_matrix * vec4(n, 0.0)).xyz;

    bs_collMeshVertexPositions[global_id].position.xyz = pos;
    bs_collMeshVertexPositions[global_id].normal.xyz = n;
}
