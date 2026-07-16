#version 450

#define THREAD_GROUP_SIZE 64
#define AMD_TRESSFX_MAX_NUM_BONES 128

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 4, std430) readonly buffer InInitialHairPositions { vec4 g_InitialHairPositions[]; };
layout(set = 0, binding = 5, std430) readonly buffer InHairRestLength { float g_HairRestLengthSRV[]; };
layout(set = 0, binding = 6, std430) readonly buffer InHairStrandType { int g_HairStrandType[]; };
layout(set = 0, binding = 7, std430) readonly buffer InFollowHairRootOffset { vec4 g_FollowHairRootOffset[]; };

struct BoneSkinningData { vec4 boneIndex; vec4 boneWeight; };
layout(set = 0, binding = 12, std430) readonly buffer InBoneSkinningData { BoneSkinningData g_BoneSkinningData[]; };

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

layout(set = 1, binding = 0, std430) buffer RWPositions { vec4 g_HairVertexPositions[]; };
layout(set = 1, binding = 1, std430) buffer RWPositionsPrev { vec4 g_HairVertexPositionsPrev[]; };
layout(set = 1, binding = 2, std430) buffer RWPositionsPrevPrev { vec4 g_HairVertexPositionsPrevPrev[]; };
layout(set = 1, binding = 3, std430) buffer RWTangents { vec4 g_HairVertexTangents[]; };

struct StrandLevelData { vec4 skinningQuat; vec4 vspQuat; vec4 vspTranslation; };
layout(set = 1, binding = 4, std430) buffer RWStrandLevelData { StrandLevelData g_StrandLevelData[]; };

bool IsMovable(vec4 particle) { return particle.w > 0.0; }

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

uint GetStrandType(uint globalStrandIndex) { return 0u; }

float GetLocalStiffness(uint strandType) { return cb.g_Shape.y; }

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

void main() {
    uint GIndex = gl_LocalInvocationIndex;
    uint group_id = gl_WorkGroupID.x;

    uint globalStrandIndex;
    uint numVerticesInTheStrand;
    uint globalRootVertexIndex;
    uint strandType;

    CalcIndicesInStrandLevelMaster(GIndex, group_id, globalStrandIndex, numVerticesInTheStrand, globalRootVertexIndex, strandType);

    float stiffnessForLocalShapeMatching = GetLocalStiffness(strandType);
    stiffnessForLocalShapeMatching = 0.5 * min(stiffnessForLocalShapeMatching, 0.95);

    vec4 boneQuat = g_StrandLevelData[globalStrandIndex].skinningQuat;

    for (uint localVertexIndex = 1u; localVertexIndex < (numVerticesInTheStrand - 1u); localVertexIndex++) {
        uint globalVertexIndex = globalRootVertexIndex + localVertexIndex;

        vec4 pos = g_HairVertexPositions[globalVertexIndex];
        vec4 pos_plus_one = g_HairVertexPositions[globalVertexIndex + 1u];
        vec4 pos_minus_one = g_HairVertexPositions[globalVertexIndex - 1u];

        vec3 bindPos = MultQuaternionAndVector(boneQuat, g_InitialHairPositions[globalVertexIndex].xyz);
        vec3 bindPos_plus_one = MultQuaternionAndVector(boneQuat, g_InitialHairPositions[globalVertexIndex + 1u].xyz);
        vec3 bindPos_minus_one = MultQuaternionAndVector(boneQuat, g_InitialHairPositions[globalVertexIndex - 1u].xyz);

        vec3 lastVec = pos.xyz - pos_minus_one.xyz;

        vec3 vecBindPose = bindPos_plus_one - bindPos;
        vec3 lastVecBindPose = bindPos - bindPos_minus_one;

        vec3 n0 = normalize(lastVecBindPose);
        vec3 n1 = normalize(lastVec);
        vec4 rotGlobal = QuatFromTwoUnitVectors(n0, n1);

        vec3 orgPos_i_plus_1_InGlobalFrame = MultQuaternionAndVector(rotGlobal, vecBindPose) + pos.xyz;
        vec3 del = stiffnessForLocalShapeMatching * (orgPos_i_plus_1_InGlobalFrame - pos_plus_one.xyz);

        if (IsMovable(pos)) {
            pos.xyz -= del;
        }

        if (IsMovable(pos_plus_one)) {
            pos_plus_one.xyz += del;
        }

        g_HairVertexPositions[globalVertexIndex].xyz = pos.xyz;
        g_HairVertexPositions[globalVertexIndex + 1u].xyz = pos_plus_one.xyz;
    }
}
