#version 450

// A3.2 subtask 4: SDF-vs-hair collision response. One thread per hair vertex;
// projects any vertex found inside the collision margin back out along the
// SDF gradient. Faithful port of
// thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl, entry point
// `CollideHairVerticesWithSdf_forward` (~532) -- the ONE AMD's own sample
// actually dispatches (TressFXSDFCollision.cpp CollideWithHair() binds
// system.m_CollideHairVerticesWithSdfPSO, which TressFXSDFCollision.h's
// Initialize() compiles from entry point "CollideHairVerticesWithSdf_forward"
// despite the misleadingly-named PSO variable). The other entry point in the
// same HLSL file, `CollideHairVerticesWithSdf` (~600, mixes forward and
// backward finite differences for speed, per AMD's own comment "could also be
// less stable"), is NOT used by AMD's sample and is not ported here (see
// audit note S3 in AUDIT_KERNELS.md).
//
// Bindings: set 0 is the SAME GenerateSDF layout the three SDF-build kernels
// use (CreateGenerateSDFLayout() in TressFXLayouts.cpp; src/SDF.cpp's shared
// m_sdfBindSet -- bindings 0/2 unused by this entry point, declared anyway
// for descriptor-set-interface consistency, same rationale as the other three
// files in this family). Set 1 is CreateApplySDFLayout()'s ApplySDF layout,
// ONE bind set PER HAIR OBJECT (created automatically in
// TressFXHairObject::CreateGPUResources -> TressFXDynamicState::
// CreateGPUResources, vendored, over that hair object's own
// Positions/PositionsPrev buffers -- the SAME buffers the six sim kernels
// read/write, per HairStrands' TransitionSimToRendering/TransitionRenderingToSim
// UAV<->SRV bookkeeping).

#define THREAD_GROUP_SIZE 64
#define INITIAL_DISTANCE 1e10

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InTrimeshVertexIndices {
    uint g_TrimeshVertexIndices[];
};

// FloatFlip-decoded back to plain IEEE-754 float bit patterns by
// FinalizeSignedDistanceField; a straight bit-cast is all that's needed here.
layout(set = 0, binding = 1, std430) buffer InSignedDistanceField {
    uint g_SignedDistanceField[];
};

struct StandardVertex {
    vec4 position;
    vec4 normal;
};

layout(set = 0, binding = 2, std430) readonly buffer InCollMeshVertexPositions {
    StandardVertex collMeshVertexPositions[];
};

layout(set = 0, binding = 3, std140) uniform ConstBuffer_SDF {
    vec4 g_Origin;
    float g_CellSize;
    int g_NumCellsX;
    int g_NumCellsY;
    int g_NumCellsZ;
    int g_MaxMarchingCubesVertices;
    float g_MarchingCubesIsolevel;
    float g_CollisionMargin;
    int g_NumHairVerticesPerStrand;
    int g_NumTotalHairVertices;
    float pad1;
    float pad2;
    float pad3;
};

// Same buffers the six audited TressFXSimulation.*.comp.glsl kernels read/
// write (TressFXDynamicState::m_Positions/m_PositionsPrev); layout order
// matches CreateApplySDFLayout() (TressFXLayouts.cpp) and the per-hair-object
// bind set TressFXHairObject::CreateGPUResources builds over exactly these
// two buffers, in this order.
layout(set = 1, binding = 0, std430) buffer InOutHairVertices {
    vec4 g_HairVertices[];
};

layout(set = 1, binding = 1, std430) buffer InOutPrevHairVertices {
    vec4 g_PrevHairVertices[];
};

// Get SDF cell index coordinates (x, y and z) from a point position in world space.
ivec3 GetSdfCoordinates(vec3 positionInWorld) {
    vec3 sdfPosition = (positionInWorld - g_Origin.xyz) / g_CellSize;
    return ivec3(int(sdfPosition.x), int(sdfPosition.y), int(sdfPosition.z));
}

vec3 GetSdfCellPosition(ivec3 gridPosition) {
    return vec3(gridPosition) * g_CellSize + g_Origin.xyz;
}

int GetSdfCellIndex(ivec3 gridPosition) {
    int cellsPerLine = g_NumCellsX;
    int cellsPerPlane = g_NumCellsX * g_NumCellsY;
    return cellsPerPlane * gridPosition.z + cellsPerLine * gridPosition.y + gridPosition.x;
}

float LinearInterpolate(float a, float b, float t) {
    return a * (1.0 - t) + b * t;
}

float BilinearInterpolate(float a, float b, float c, float d, float p, float q) {
    return LinearInterpolate(LinearInterpolate(a, b, p), LinearInterpolate(c, d, p), q);
}

float TrilinearInterpolate(float a, float b, float c, float d, float e, float f, float g, float h, float p, float q, float r) {
    return LinearInterpolate(BilinearInterpolate(a, b, c, d, p, q), BilinearInterpolate(e, f, g, h, p, q), r);
}

// Get signed distance at the position in world space (trilinear interpolation
// of the 8 surrounding grid cells; INITIAL_DISTANCE -- the sentinel written by
// InitializeSignedDistanceField for cells no triangle ever reached, or for
// positions outside the grid entirely -- propagates unchanged so callers can
// treat it as "far away, no collision").
float GetSignedDistance(vec3 positionInWorld) {
    ivec3 gridCoords = GetSdfCoordinates(positionInWorld);

    if (!(0 <= gridCoords.x && gridCoords.x < g_NumCellsX - 2)
     || !(0 <= gridCoords.y && gridCoords.y < g_NumCellsY - 2)
     || !(0 <= gridCoords.z && gridCoords.z < g_NumCellsZ - 2)) {
        return INITIAL_DISTANCE;
    }

    int sdfIndices[8];
    {
        int index = GetSdfCellIndex(gridCoords);
        for (int i = 0; i < 8; ++i) sdfIndices[i] = index;

        int x = 1;
        int y = g_NumCellsX;
        int z = g_NumCellsY * g_NumCellsX;

        sdfIndices[1] += x;
        sdfIndices[2] += y;
        sdfIndices[3] += y + x;

        sdfIndices[4] += z;
        sdfIndices[5] += z + x;
        sdfIndices[6] += z + y;
        sdfIndices[7] += z + y + x;
    }

    float distances[8];
    for (int j = 0; j < 8; ++j) {
        float dist = uintBitsToFloat(g_SignedDistanceField[sdfIndices[j]]);
        if (dist == INITIAL_DISTANCE) {
            return INITIAL_DISTANCE;
        }
        distances[j] = dist;
    }

    vec3 cellPosition = GetSdfCellPosition(gridCoords);
    vec3 interp = (positionInWorld - cellPosition) / g_CellSize;
    return TrilinearInterpolate(distances[0], distances[1], distances[2], distances[3],
                                 distances[4], distances[5], distances[6], distances[7],
                                 interp.x, interp.y, interp.z);
}

void main() {
    int hairVertexGlobalIndex = int(gl_GlobalInvocationID.x);
    if (hairVertexGlobalIndex >= g_NumTotalHairVertices) {
        return;
    }

    int hairVertexLocalIndex = hairVertexGlobalIndex % g_NumHairVerticesPerStrand;

    // We don't run collision checks on the first two vertices in the strand --
    // they are fixed to the skin mesh (AMD's own comment, verbatim).
    if (hairVertexLocalIndex == 0 || hairVertexLocalIndex == 1) {
        return;
    }

    vec4 hairVertex = g_HairVertices[hairVertexGlobalIndex];
    vec3 vertexInSdfLocalSpace = hairVertex.xyz;

    float dist = GetSignedDistance(vertexInSdfLocalSpace);

    // Early exit if the distance is larger than the collision margin.
    if (dist > g_CollisionMargin) {
        return;
    }

    // Small displacement for the forward-difference gradient estimate.
    float h = 0.1 * g_CellSize;

    vec3 forwardDistances;
    forwardDistances.x = GetSignedDistance(vertexInSdfLocalSpace + vec3(h, 0.0, 0.0));
    forwardDistances.y = GetSignedDistance(vertexInSdfLocalSpace + vec3(0.0, h, 0.0));
    forwardDistances.z = GetSignedDistance(vertexInSdfLocalSpace + vec3(0.0, 0.0, h));

    vec3 sdfGradient = (forwardDistances - vec3(dist)) / h;

    // Project the hair vertex back out of the SDF.
    vec3 normal = normalize(sdfGradient);

    if (dist < g_CollisionMargin) {
        vec3 projectedVertex = hairVertex.xyz + normal * (g_CollisionMargin - dist);
        g_HairVertices[hairVertexGlobalIndex].xyz = projectedVertex;
        g_PrevHairVertices[hairVertexGlobalIndex].xyz = projectedVertex;
    }
}
