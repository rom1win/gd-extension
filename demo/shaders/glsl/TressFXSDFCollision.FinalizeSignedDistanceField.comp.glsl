#version 450

// A3.2 subtask 3: undoes the FloatFlip3 encoding after all triangles have
// splatted their (atomic-min) distances, restoring g_SignedDistanceField to
// plain IEEE-754 float bit patterns for downstream consumers (e.g. the
// collide-hair kernel, subtask 4) to read with a simple bit-cast. Faithful
// port of thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl, entry
// point `FinalizeSignedDistanceField` (line ~401): one thread per grid cell.
//
// See TressFXSDFCollision.InitializeSignedDistanceField.comp.glsl for the
// binding-layout rationale (identical here; this entry point only touches
// binding 1 and the UBO's grid-dimension fields).

#define THREAD_GROUP_SIZE 64

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InTrimeshVertexIndices {
    uint g_TrimeshVertexIndices[];
};

layout(set = 0, binding = 1, std430) buffer InOutSignedDistanceField {
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

// Inverse of FloatFlip3 (see ConstructSignedDistanceField.comp.glsl): undoes
// the sign-bit rotation so the stored uint is once again a plain bit-cast of
// the original float distance.
uint IFloatFlip3(uint f2) {
    return (f2 >> 1u) | (f2 << 31u);
}

void main() {
    int numSdfCells = g_NumCellsX * g_NumCellsY * g_NumCellsZ;
    int sdfCellIndex = int(gl_GlobalInvocationID.x);
    if (sdfCellIndex >= numSdfCells) {
        return;
    }

    uint distanceEncoded = g_SignedDistanceField[sdfCellIndex];
    g_SignedDistanceField[sdfCellIndex] = IFloatFlip3(distanceEncoded);
}
