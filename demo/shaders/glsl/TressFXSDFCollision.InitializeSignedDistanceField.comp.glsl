#version 450

// A3.2 subtask 3: resets every SDF grid cell to a large "no data yet"
// distance before ConstructSignedDistanceField splats triangle distances in.
// Faithful port of thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl,
// entry point `InitializeSignedDistanceField` (line ~302): one thread per
// grid cell.
//
// Bindings: own private descriptor set (set 0), matching
// CreateGenerateSDFLayout() in TressFXLayouts.cpp (still compiled into this
// build, reused by src/SDF.cpp for all three SDF-build kernels) and AMD's
// original HLSL `[[vk::binding(n, 0)]]` annotations for this shader family.
// All 4 bindings are declared in every one of the three split-out kernel
// files (even where an entry point doesn't touch one) so the compiled SPIR-V
// keeps a consistent descriptor-set interface across all three PSOs, which
// share ONE EI_BindSet (src/SDF.cpp EnsureSDFPSOCreated) -- this mirrors
// AMD's own scheme, where all these entry points (plus CollideHairVerticesWithSdf)
// are compiled from the same TressFXSDFCollision.hlsl file and see the same
// file-scope resource declarations regardless of which ones they use:
//   binding 0 (RO)      g_TrimeshVertexIndices  -- unused by this entry point
//   binding 1 (RW)      g_SignedDistanceField   -- uint grid (FloatFlip-encoded)
//   binding 2 (RO)      collMeshVertexPositions -- unused by this entry point
//   binding 3 (uniform) ConstBuffer_SDF

#define THREAD_GROUP_SIZE 64
#define INITIAL_DISTANCE 1e10

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InTrimeshVertexIndices {
    uint g_TrimeshVertexIndices[];
};

// Actually contains floats; make sure to bit-cast (floatBitsToUint/
// uintBitsToFloat) when accessing. uint is used to allow atomics -- there is
// no 32-bit float atomic_min, so FloatFlip3 remaps the float's bit pattern
// into a monotonically-ordered uint that atomicMin can operate on correctly
// (see ConstructSignedDistanceField, which performs the actual atomic-min
// splatting; this entry point only writes the encoded initial value).
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

uint FloatFlip3(float fl) {
    uint f = floatBitsToUint(fl);
    return (f << 1u) | (f >> 31u); // Rotate sign bit to least significant.
}

void main() {
    int numSdfCells = g_NumCellsX * g_NumCellsY * g_NumCellsZ;
    int sdfCellIndex = int(gl_GlobalInvocationID.x);
    if (sdfCellIndex >= numSdfCells) {
        return;
    }

    g_SignedDistanceField[sdfCellIndex] = FloatFlip3(INITIAL_DISTANCE);
}
