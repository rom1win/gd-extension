#version 450

#define THREAD_GROUP_SIZE 64
#define AMD_TRESSFX_MAX_NUM_BONES 128
#define TRESSFX_MAX_NUM_COLLISION_CAPSULES 8

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

// Set 0: static SRVs + simulation constant buffer
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

    vec4 g_Shape;       // damping, local stiffness, global stiffness, global range
    vec4 g_GravTimeTip; // gravity magnitude, time step, tip separation, unused
    ivec4 g_SimInts;    // length iterations, local iterations, collision flag, unused
    ivec4 g_Counts;     // strands per thread group, follow hairs per guide, verts per strand
    vec4 g_VSP;         // vsp parameters

    float g_ResetPositions;
    float g_ClampPositionDelta;
    float g_pad1;
    float g_pad2;

    // NOTE: In the original HLSL this is a row_major float4x4 array.
    // We intentionally treat it as column-major here (GLSL default).
    // When applying transforms we multiply as (M * v) to match HLSL's mul(v, row_major_M).
    mat4 g_BoneSkinningMatrix[AMD_TRESSFX_MAX_NUM_BONES];

    // A3.2: capsule collision (inert while g_numCollisionCapsules.x == 0).
    vec4 g_centerAndRadius0[TRESSFX_MAX_NUM_COLLISION_CAPSULES];
    vec4 g_centerAndRadius1[TRESSFX_MAX_NUM_COLLISION_CAPSULES];
    ivec4 g_numCollisionCapsules;
} cb;

// Set 1: RW buffers for simulation state
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

shared vec4 sharedPos[THREAD_GROUP_SIZE];
shared vec4 sharedTangent[THREAD_GROUP_SIZE];
shared float sharedLength[THREAD_GROUP_SIZE];

bool IsMovable(vec4 particle) {
    return particle.w > 0.0;
}

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

uint GetStrandType(uint globalStrandIndex) {
    // Strand types are currently unused in upstream TressFX simulation.
    return 0u;
}

float GetDamping(uint strandType) {
    return cb.g_Shape.x;
}

float GetGlobalStiffness(uint strandType) {
    return cb.g_Shape.z;
}

float GetGlobalRange(uint strandType) {
    return cb.g_Shape.w;
}

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

vec3 Integrate(vec3 curPosition, vec3 oldPosition, vec3 initialPos, float dampingCoeff) {
    const float gravityMagnitude = cb.g_GravTimeTip.x;
    const float timeStep = cb.g_GravTimeTip.y;

    vec3 force = gravityMagnitude * vec3(0.0, -1.0, 0.0);
    float decay = exp(-dampingCoeff * timeStep * 60.0);
    return curPosition + decay * (curPosition - oldPosition) + force * timeStep * timeStep;
}

vec3 ApplyVertexBoneSkinning(vec3 vertexPos, BoneSkinningData skinningData, out vec4 bone_quat) {
    // Dual-quaternion path is disabled in the upstream defaults (and here).
    bone_quat = vec4(0.0, 0.0, 0.0, 1.0);

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

    // Multiply as (M * v) to match HLSL mul(v, row_major_M) given the differing default matrix layouts.
    return (bone_matrix * vec4(vertexPos, 1.0)).xyz;
}

void UpdateFinalVertexPositions(vec4 oldPosition, vec4 newPosition, uint globalVertexIndex) {
    g_HairVertexPositionsPrevPrev[globalVertexIndex] = g_HairVertexPositionsPrev[globalVertexIndex];
    g_HairVertexPositionsPrev[globalVertexIndex] = oldPosition;
    g_HairVertexPositions[globalVertexIndex] = newPosition;
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

    vec4 initialPos = g_InitialHairPositions[globalVertexIndex];

    // Apply bone skinning to initial position.
    vec4 bone_quat;
    BoneSkinningData skinningData = g_BoneSkinningData[globalStrandIndex];
    initialPos.xyz = ApplyVertexBoneSkinning(initialPos.xyz, skinningData, bone_quat);

    vec4 currentPos = g_HairVertexPositions[globalVertexIndex];
    sharedPos[indexForSharedMem] = currentPos;
    group_sync();

    float dampingCoeff = GetDamping(strandType);
    vec4 oldPos = g_HairVertexPositionsPrev[globalVertexIndex];

    if (cb.g_ResetPositions != 0.0) {
        currentPos = initialPos;
        oldPos = initialPos;
        sharedPos[indexForSharedMem] = initialPos;
    }

    if (IsMovable(currentPos)) {
        sharedPos[indexForSharedMem].xyz = Integrate(currentPos.xyz, oldPos.xyz, initialPos.xyz, dampingCoeff);
    } else {
        sharedPos[indexForSharedMem] = initialPos;
    }

    // Global shape constraints
    float stiffnessForGlobalShapeMatching = GetGlobalStiffness(strandType);
    float globalShapeMatchingEffectiveRange = GetGlobalRange(strandType);

    if (stiffnessForGlobalShapeMatching > 0.0 && globalShapeMatchingEffectiveRange > 0.0) {
        if (IsMovable(sharedPos[indexForSharedMem])) {
            if (float(localVertexIndex) < globalShapeMatchingEffectiveRange * float(numVerticesInTheStrand)) {
                vec3 del = stiffnessForGlobalShapeMatching * (initialPos.xyz - sharedPos[indexForSharedMem].xyz);
                sharedPos[indexForSharedMem].xyz += del;
            }
        }
    }

    UpdateFinalVertexPositions(currentPos, sharedPos[indexForSharedMem], globalVertexIndex);
}
