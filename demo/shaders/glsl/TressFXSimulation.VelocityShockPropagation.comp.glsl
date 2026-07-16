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

vec3 MultQuaternionAndVector(vec4 q, vec3 v) {
    vec3 qvec = q.xyz;
    vec3 uv = cross(qvec, v);
    vec3 uuv = cross(qvec, uv);
    uv *= (2.0 * q.w);
    uuv *= 2.0;
    return v + uv + uuv;
}

uint GetStrandType(uint globalStrandIndex) { return 0u; }

void CalcIndicesInVertexLevelMaster(
    uint local_id,
    uint group_id,
    out uint globalStrandIndex,
    out uint localStrandIndex,
    out uint globalVertexIndex,
    out uint localVertexIndex,
    out uint numVerticesInTheStrand,
    out uint indexForSharedMem,
    out uint strandType
) {
    indexForSharedMem = local_id;

    const uint numOfStrandsPerThreadGroup = uint(cb.g_Counts.x);
    const uint numFollowHairsPerGuideHair = uint(cb.g_Counts.y);

    numVerticesInTheStrand = THREAD_GROUP_SIZE / max(numOfStrandsPerThreadGroup, 1u);

    localStrandIndex = local_id % max(numOfStrandsPerThreadGroup, 1u);
    globalStrandIndex = group_id * max(numOfStrandsPerThreadGroup, 1u) + localStrandIndex;
    globalStrandIndex *= (numFollowHairsPerGuideHair + 1u);

    localVertexIndex = (local_id - localStrandIndex) / max(numOfStrandsPerThreadGroup, 1u);
    strandType = GetStrandType(globalStrandIndex);
    globalVertexIndex = globalStrandIndex * numVerticesInTheStrand + localVertexIndex;
}

void main() {
    uint GIndex = gl_LocalInvocationIndex;
    uint group_id = gl_WorkGroupID.x;

    uint globalStrandIndex;
    uint localStrandIndex;
    uint globalVertexIndex;
    uint localVertexIndex;
    uint numVerticesInTheStrand;
    uint indexForSharedMem;
    uint strandType;

    CalcIndicesInVertexLevelMaster(
        GIndex,
        group_id,
        globalStrandIndex,
        localStrandIndex,
        globalVertexIndex,
        localVertexIndex,
        numVerticesInTheStrand,
        indexForSharedMem,
        strandType
    );

    if (localVertexIndex < 2u) {
        return;
    }

    vec4 vspQuat = g_StrandLevelData[globalStrandIndex].vspQuat;
    vec4 vspTrans = g_StrandLevelData[globalStrandIndex].vspTranslation;
    float vspCoeff = vspTrans.w;

    vec4 pos_new_n = g_HairVertexPositions[globalVertexIndex];
    vec4 pos_old_n = g_HairVertexPositionsPrev[globalVertexIndex];

    vec3 warped_new = MultQuaternionAndVector(vspQuat, pos_new_n.xyz) + vspTrans.xyz;
    vec3 warped_old = MultQuaternionAndVector(vspQuat, pos_old_n.xyz) + vspTrans.xyz;

    pos_new_n.xyz = mix(pos_new_n.xyz, warped_new, vspCoeff);
    pos_old_n.xyz = mix(pos_old_n.xyz, warped_old, vspCoeff);

    g_HairVertexPositions[globalVertexIndex].xyz = pos_new_n.xyz;
    g_HairVertexPositionsPrev[globalVertexIndex].xyz = pos_old_n.xyz;
}
