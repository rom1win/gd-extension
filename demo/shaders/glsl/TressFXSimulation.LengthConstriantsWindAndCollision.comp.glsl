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

shared vec4 sharedPos[THREAD_GROUP_SIZE];
shared vec4 sharedTangent[THREAD_GROUP_SIZE];
shared float sharedLength[THREAD_GROUP_SIZE];

bool IsMovable(vec4 particle) { return particle.w > 0.0; }

vec2 ConstraintMultiplier(vec4 particle0, vec4 particle1) {
    const bool m0 = IsMovable(particle0);
    const bool m1 = IsMovable(particle1);
    if (m0) {
        return m1 ? vec2(0.5, 0.5) : vec2(1.0, 0.0);
    }
    return m1 ? vec2(0.0, 1.0) : vec2(0.0, 0.0);
}

void ApplyDistanceConstraint(inout vec4 pos0, inout vec4 pos1, float targetDistance, float stiffness) {
    vec3 delta = pos1.xyz - pos0.xyz;
    float distance = max(length(delta), 1e-7);
    float stretching = 1.0 - targetDistance / distance;
    delta = stretching * delta;
    vec2 mult = ConstraintMultiplier(pos0, pos1);

    pos0.xyz += mult.x * delta * stiffness;
    pos1.xyz -= mult.y * delta * stiffness;
}

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

    uint numOfStrandsPerThreadGroup = uint(cb.g_Counts.x);

    sharedPos[indexForSharedMem] = g_HairVertexPositions[globalVertexIndex];
    sharedLength[indexForSharedMem] = g_HairRestLengthSRV[globalVertexIndex];
    group_sync();

    // Wind
    vec3 wind0 = cb.g_Wind.xyz;
    if (dot(wind0, wind0) > 0.0) {
        if (localVertexIndex >= 2u && localVertexIndex < (numVerticesInTheStrand - 1u)) {
            float a = float(globalStrandIndex % 20u) / 20.0;
            vec3 w = a * cb.g_Wind.xyz + (1.0 - a) * cb.g_Wind1.xyz + a * cb.g_Wind2.xyz + (1.0 - a) * cb.g_Wind3.xyz;

            uint sharedIndex = localVertexIndex * numOfStrandsPerThreadGroup + localStrandIndex;

            vec3 v = sharedPos[sharedIndex].xyz - sharedPos[sharedIndex + numOfStrandsPerThreadGroup].xyz;
            vec3 force = -cross(cross(v, w), v);
            float dt = cb.g_GravTimeTip.y;
            sharedPos[sharedIndex].xyz += force * dt * dt;
        }
    }

    group_sync();

    // Enforce length constraints
    uint aCount = numVerticesInTheStrand / 2u;
    uint bCount = (numVerticesInTheStrand - 1u) / 2u;

    int nLengthContraintIterations = cb.g_SimInts.x;

    for (int iterationE = 0; iterationE < nLengthContraintIterations; iterationE++) {
        uint sharedIndex = 2u * localVertexIndex * numOfStrandsPerThreadGroup + localStrandIndex;

        if (localVertexIndex < aCount) {
            ApplyDistanceConstraint(sharedPos[sharedIndex], sharedPos[sharedIndex + numOfStrandsPerThreadGroup], sharedLength[sharedIndex], 1.0);
        }

        group_sync();

        if (localVertexIndex < bCount) {
            ApplyDistanceConstraint(
                sharedPos[sharedIndex + numOfStrandsPerThreadGroup],
                sharedPos[sharedIndex + numOfStrandsPerThreadGroup * 2u],
                sharedLength[sharedIndex + numOfStrandsPerThreadGroup],
                1.0
            );
        }

        group_sync();
    }

    // Collision: capsule collision is disabled in the current upstream defaults.
    vec4 oldPos = g_HairVertexPositionsPrev[globalVertexIndex];
    bool bAnyColDetected = false;

    group_sync();

    // Compute tangent
    uint indexForTangent = (localVertexIndex == (numVerticesInTheStrand - 1u)) ? (indexForSharedMem - numOfStrandsPerThreadGroup) : indexForSharedMem;
    vec3 tangent = sharedPos[indexForTangent + numOfStrandsPerThreadGroup].xyz - sharedPos[indexForTangent].xyz;
    g_HairVertexTangents[globalVertexIndex].xyz = normalize(tangent);

    // Clamp velocities, rewrite history
    vec3 positionDelta = sharedPos[indexForSharedMem].xyz - oldPos.xyz;
    float speedSqr = dot(positionDelta, positionDelta);
    float clampDelta = cb.g_ClampPositionDelta;

    if (speedSqr > clampDelta * clampDelta) {
        positionDelta *= (clampDelta * clampDelta) / speedSqr;
        g_HairVertexPositionsPrev[globalVertexIndex].xyz = sharedPos[indexForSharedMem].xyz - positionDelta;
    }

    // Update global position buffers
    g_HairVertexPositions[globalVertexIndex] = sharedPos[indexForSharedMem];
    if (bAnyColDetected) {
        g_HairVertexPositionsPrev[globalVertexIndex] = sharedPos[indexForSharedMem];
    }
}
