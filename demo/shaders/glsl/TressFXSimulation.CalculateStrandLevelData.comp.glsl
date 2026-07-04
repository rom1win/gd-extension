#version 450

#define THREAD_GROUP_SIZE 64
#define AMD_TRESSFX_MAX_NUM_BONES 128

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 4, std430) readonly buffer InInitialHairPositions {
    vec4 g_InitialHairPositions[];
};

layout(set = 0, binding = 5, std430) readonly buffer InHairRestLength {
    float g_HairRestLengthSRV[];
};

layout(set = 0, binding = 6, std430) readonly buffer InHairStrandType {
    int g_HairStrandType[];
};

layout(set = 0, binding = 7, std430) readonly buffer InFollowHairRootOffset {
    vec4 g_FollowHairRootOffset[];
};

struct BoneSkinningData {
    vec4 boneIndex;
    vec4 boneWeight;
};

layout(set = 0, binding = 12, std430) readonly buffer InBoneSkinningData {
    BoneSkinningData g_BoneSkinningData[];
};

layout(set = 0, binding = 13, std140) uniform tressfxSimParameters {
    vec4 g_Wind;
    vec4 g_Wind1;
    vec4 g_Wind2;
    vec4 g_Wind3;

    vec4 g_Shape;
    vec4 g_GravTimeTip;
    ivec4 g_SimInts;
    ivec4 g_Counts;
    vec4 g_VSP;

    float g_ResetPositions;
    float g_ClampPositionDelta;
    float g_pad1;
    float g_pad2;

    mat4 g_BoneSkinningMatrix[AMD_TRESSFX_MAX_NUM_BONES];
} cb;

layout(set = 1, binding = 0, std430) buffer RWPositions {
    vec4 g_HairVertexPositions[];
};

layout(set = 1, binding = 1, std430) buffer RWPositionsPrev {
    vec4 g_HairVertexPositionsPrev[];
};

layout(set = 1, binding = 2, std430) buffer RWPositionsPrevPrev {
    vec4 g_HairVertexPositionsPrevPrev[];
};

layout(set = 1, binding = 3, std430) buffer RWTangents {
    vec4 g_HairVertexTangents[];
};

struct StrandLevelData {
    vec4 skinningQuat;
    vec4 vspQuat;
    vec4 vspTranslation;
};

layout(set = 1, binding = 4, std430) buffer RWStrandLevelData {
    StrandLevelData g_StrandLevelData[];
};

vec4 normalize_quat(vec4 q) {
    float n = dot(q, q);
    if (n < 1e-10) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    return q * inversesqrt(n);
}

vec4 QuatFromTwoUnitVectors(vec3 u, vec3 v) {
    float r = 1.0 + dot(u, v);
    vec3 n;

    if (r < 1e-7) {
        r = 0.0;
        n = (abs(u.x) > abs(u.z)) ? vec3(-u.y, u.x, 0.0) : vec3(0.0, -u.z, u.y);
    } else {
        n = cross(u, v);
    }

    return normalize_quat(vec4(n, r));
}

vec3 MultQuaternionAndVector(vec4 q, vec3 v) {
    vec3 qvec = q.xyz;
    vec3 uv = cross(qvec, v);
    vec3 uuv = cross(qvec, uv);
    uv *= (2.0 * q.w);
    uuv *= 2.0;
    return v + uv + uuv;
}

uint GetStrandType(uint globalStrandIndex) {
    return 0u;
}

void CalcIndicesInStrandLevelMaster(
    uint local_id,
    uint group_id,
    out uint globalStrandIndex,
    out uint numVerticesInTheStrand,
    out uint globalRootVertexIndex,
    out uint strandType
) {
    const uint numOfStrandsPerThreadGroup = uint(cb.g_Counts.x);
    const uint numFollowHairsPerGuideHair = uint(cb.g_Counts.y);

    globalStrandIndex = THREAD_GROUP_SIZE * group_id + local_id;
    globalStrandIndex *= (numFollowHairsPerGuideHair + 1u);

    numVerticesInTheStrand = THREAD_GROUP_SIZE / max(numOfStrandsPerThreadGroup, 1u);
    strandType = GetStrandType(globalStrandIndex);
    globalRootVertexIndex = globalStrandIndex * numVerticesInTheStrand;
}

// Extract rotation quaternion from a blended bone matrix.
// The CPU stores bone matrices as XMMATRIX (row-major) which GLSL reads as column-major mat4.
// With this reading, M_glsl[col][row] = M_cpu_row_col, so M_glsl * v_col produces the same
// geometric transform as HLSL mul(v_row, M_hlsl_row_major). The 3x3 rotation trace and
// off-diagonal elements are in M_glsl[0..2][0..2], matching the column-major convention.
vec4 MatToQuat(mat4 m) {
    float trace = m[0][0] + m[1][1] + m[2][2];
    vec4 q;
    if (trace > 0.0) {
        float r = sqrt(trace + 1.0);
        q.w = 0.5 * r;
        r = 0.5 / r;
        q.x = (m[1][2] - m[2][1]) * r;
        q.y = (m[2][0] - m[0][2]) * r;
        q.z = (m[0][1] - m[1][0]) * r;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float r = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]);
        float s = 0.5 / r;
        q.x = 0.5 * r;
        q.y = (m[1][0] + m[0][1]) * s;
        q.z = (m[0][2] + m[2][0]) * s;
        q.w = (m[1][2] - m[2][1]) * s;
    } else if (m[1][1] > m[2][2]) {
        float r = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]);
        float s = 0.5 / r;
        q.x = (m[1][0] + m[0][1]) * s;
        q.y = 0.5 * r;
        q.z = (m[2][1] + m[1][2]) * s;
        q.w = (m[2][0] - m[0][2]) * s;
    } else {
        float r = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]);
        float s = 0.5 / r;
        q.x = (m[0][2] + m[2][0]) * s;
        q.y = (m[2][1] + m[1][2]) * s;
        q.z = 0.5 * r;
        q.w = (m[0][1] - m[1][0]) * s;
    }
    return normalize_quat(q);
}

vec3 ApplyVertexBoneSkinning(vec3 vertexPos, BoneSkinningData skinningData, out vec4 bone_quat) {
    int idx0 = int(skinningData.boneIndex.x);
    int idx1 = int(skinningData.boneIndex.y);
    int idx2 = int(skinningData.boneIndex.z);
    int idx3 = int(skinningData.boneIndex.w);

    float w0 = skinningData.boneWeight.x;
    float w1 = skinningData.boneWeight.y;
    float w2 = skinningData.boneWeight.z;
    float w3 = skinningData.boneWeight.w;

    float weight_sum = w0;
    mat4 bone_matrix = cb.g_BoneSkinningMatrix[clamp(idx0, 0, AMD_TRESSFX_MAX_NUM_BONES - 1)] * w0;

    if (w1 > 0.0) { bone_matrix += cb.g_BoneSkinningMatrix[clamp(idx1, 0, AMD_TRESSFX_MAX_NUM_BONES - 1)] * w1; weight_sum += w1; }
    if (w2 > 0.0) { bone_matrix += cb.g_BoneSkinningMatrix[clamp(idx2, 0, AMD_TRESSFX_MAX_NUM_BONES - 1)] * w2; weight_sum += w2; }
    if (w3 > 0.0) { bone_matrix += cb.g_BoneSkinningMatrix[clamp(idx3, 0, AMD_TRESSFX_MAX_NUM_BONES - 1)] * w3; weight_sum += w3; }

    if (weight_sum > 1e-6) {
        bone_matrix /= weight_sum;
    }

    // Extract rotation quaternion for LocalShapeConstraints tracking.
    bone_quat = MatToQuat(bone_matrix);

    return (bone_matrix * vec4(vertexPos, 1.0)).xyz;
}

void main() {
    uint GIndex = gl_LocalInvocationIndex;
    uint group_id = gl_WorkGroupID.x;

    uint globalStrandIndex;
    uint numVerticesInTheStrand;
    uint globalRootVertexIndex;
    uint strandType;

    CalcIndicesInStrandLevelMaster(GIndex, group_id, globalStrandIndex, numVerticesInTheStrand, globalRootVertexIndex, strandType);

    vec4 pos_old_old0 = g_HairVertexPositionsPrevPrev[globalRootVertexIndex];
    vec4 pos_old_old1 = g_HairVertexPositionsPrevPrev[globalRootVertexIndex + 1u];

    vec4 pos_old0 = g_HairVertexPositionsPrev[globalRootVertexIndex];
    vec4 pos_old1 = g_HairVertexPositionsPrev[globalRootVertexIndex + 1u];

    vec4 pos_new0 = g_HairVertexPositions[globalRootVertexIndex];
    vec4 pos_new1 = g_HairVertexPositions[globalRootVertexIndex + 1u];

    vec3 u = normalize(pos_old1.xyz - pos_old0.xyz);
    vec3 v = normalize(pos_new1.xyz - pos_new0.xyz);

    vec4 rot = QuatFromTwoUnitVectors(u, v);
    vec3 trans = pos_new0.xyz - MultQuaternionAndVector(rot, pos_old0.xyz);

    float vspCoeff = cb.g_VSP.x;
    float vspAccelThreshold = cb.g_VSP.y;

    vec3 accelVec = pos_new1.xyz - 2.0 * pos_old1.xyz + pos_old_old1.xyz;
    float accel = length(accelVec);
    if (accel > vspAccelThreshold) {
        vspCoeff = 1.0;
    }

    g_StrandLevelData[globalStrandIndex].vspQuat = rot;
    g_StrandLevelData[globalStrandIndex].vspTranslation = vec4(trans, vspCoeff);

    vec4 initialPos = g_InitialHairPositions[globalRootVertexIndex];

    BoneSkinningData skinningData = g_BoneSkinningData[globalStrandIndex];
    vec4 bone_quat;
    initialPos.xyz = ApplyVertexBoneSkinning(initialPos.xyz, skinningData, bone_quat);

    g_StrandLevelData[globalStrandIndex].skinningQuat = bone_quat;
}
