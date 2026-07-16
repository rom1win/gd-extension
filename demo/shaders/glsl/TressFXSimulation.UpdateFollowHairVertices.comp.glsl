#version 450

#define THREAD_GROUP_SIZE 64
#define AMD_TRESSFX_MAX_NUM_BONES 128
#define TRESSFX_MAX_NUM_COLLISION_CAPSULES 8

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

    // A3.2: capsule collision (inert while g_numCollisionCapsules.x == 0).
    vec4 g_centerAndRadius0[TRESSFX_MAX_NUM_COLLISION_CAPSULES];
    vec4 g_centerAndRadius1[TRESSFX_MAX_NUM_COLLISION_CAPSULES];
    ivec4 g_numCollisionCapsules;
} cb;

layout(set = 1, binding = 0, std430) buffer RWPositions { vec4 g_HairVertexPositions[]; };
layout(set = 1, binding = 1, std430) buffer RWPositionsPrev { vec4 g_HairVertexPositionsPrev[]; };
layout(set = 1, binding = 2, std430) buffer RWPositionsPrevPrev { vec4 g_HairVertexPositionsPrevPrev[]; };
layout(set = 1, binding = 3, std430) buffer RWTangents { vec4 g_HairVertexTangents[]; };

struct StrandLevelData { vec4 skinningQuat; vec4 vspQuat; vec4 vspTranslation; };
layout(set = 1, binding = 4, std430) buffer RWStrandLevelData { StrandLevelData g_StrandLevelData[]; };

shared vec4 sharedPos[THREAD_GROUP_SIZE];
shared vec4 sharedTangent[THREAD_GROUP_SIZE];

uint GetStrandType(uint globalStrandIndex) { return 0u; }

void group_sync() {
    memoryBarrierShared();
    barrier();
}

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

    sharedPos[indexForSharedMem] = g_HairVertexPositions[globalVertexIndex];
    sharedTangent[indexForSharedMem] = g_HairVertexTangents[globalVertexIndex];
    group_sync();

    uint numFollow = uint(cb.g_Counts.y);
    float tipSep = cb.g_GravTimeTip.z;

    for (uint i = 0u; i < numFollow; i++) {
        uint globalFollowVertexIndex = globalVertexIndex + numVerticesInTheStrand * (i + 1u);
        uint globalFollowStrandIndex = globalStrandIndex + i + 1u;

        float factor = tipSep * (float(localVertexIndex) / float(numVerticesInTheStrand)) + 1.0;
        vec3 followPos = sharedPos[indexForSharedMem].xyz + factor * g_FollowHairRootOffset[globalFollowStrandIndex].xyz;

        g_HairVertexPositions[globalFollowVertexIndex].xyz = followPos;
        g_HairVertexTangents[globalFollowVertexIndex] = sharedTangent[indexForSharedMem];
    }
}
