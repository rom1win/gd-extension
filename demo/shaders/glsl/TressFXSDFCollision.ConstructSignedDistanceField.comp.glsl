#version 450

// A3.2 subtask 3: builds the signed distance to the collision mesh surface,
// one triangle at a time, splatting into every grid cell within its (padded)
// bounding box via an atomic-min (see FloatFlip3 below). Faithful port of
// thirdparty/tressfx/src/Shaders/TressFXSDFCollision.hlsl, entry point
// `ConstructSignedDistanceField` (line ~330) plus the geometry helpers it
// calls: GetSdfCoordinates/GetSdfCellPosition/GetSdfCellIndex (~102-129),
// DistancePointToEdge (~147), SignedDistancePointToTriangle (~163). One
// thread per mesh triangle.
//
// See TressFXSDFCollision.InitializeSignedDistanceField.comp.glsl for the
// binding-layout rationale (identical here; this is the only one of the
// three entry points that uses all 4 bindings).

#define THREAD_GROUP_SIZE 64
#define GRID_MARGIN ivec3(1, 1, 1)

layout(local_size_x = THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InTrimeshVertexIndices {
    uint g_TrimeshVertexIndices[];
};

// Actually contains floats; see FloatFlip3 below (no 32-bit float atomic_min,
// so the bit pattern is remapped into a monotonically-ordered uint first).
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

// When building the SDF we want to find the lowest distance at each SDF
// cell. In order to allow multiple threads to write to the same cell, it is
// necessary to use atomics; there is no atomic_min for 32-bit floats, so we
// convert the float into an unsigned int and use atomicMin() as a
// workaround. FloatFlip3() stores the float with the lowest magnitude when
// used with atomicMin, preferring positive values -- AMD's own comment notes
// this gives a higher-quality SDF than the sign-preferring FloatFlip2()
// (not ported: unused by any entry point we call).
uint FloatFlip3(float fl) {
    uint f = floatBitsToUint(fl);
    return (f << 1u) | (f >> 31u);
}

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

float DistancePointToEdge(vec3 p, vec3 x0, vec3 x1, out vec3 n) {
    vec3 x10 = x1 - x0;

    float t = dot(x1 - p, x10) / dot(x10, x10);
    t = clamp(t, 0.0, 1.0);

    vec3 a = p - (t * x0 + (1.0 - t) * x1);
    float d = length(a);
    n = a / (d + 1e-30);

    return d;
}

// Check if p is in the positive or negative side of triangle (x0, x1, x2).
// Positive side is where the normal vector of the triangle
// ((x1-x0) x (x2-x0)) is pointing to.
float SignedDistancePointToTriangle(vec3 p, vec3 x0, vec3 x1, vec3 x2) {
    float d = 0.0;
    vec3 x02 = x0 - x2;
    float l0 = length(x02) + 1e-30;
    x02 = x02 / l0;
    vec3 x12 = x1 - x2;
    float l1 = dot(x12, x02);
    x12 = x12 - l1 * x02;
    float l2 = length(x12) + 1e-30;
    x12 = x12 / l2;
    vec3 px2 = p - x2;

    float b = dot(x12, px2) / l2;
    float a = (dot(x02, px2) - l1 * b) / l0;
    float c = 1.0 - a - b;

    // normal vector of triangle. Don't need to normalize this yet.
    vec3 nTri = cross(x1 - x0, x2 - x0);
    vec3 n;

    float tol = 1e-8;

    if (a >= -tol && b >= -tol && c >= -tol) {
        n = p - (a * x0 + b * x1 + c * x2);
        d = length(n);

        vec3 n1 = n / d;
        vec3 n2 = nTri / (length(nTri) + 1e-30); // if d == 0
        n = (d > 0.0) ? n1 : n2;
    } else {
        vec3 n_12;
        vec3 n_02;
        d = DistancePointToEdge(p, x0, x1, n);

        float d12 = DistancePointToEdge(p, x1, x2, n_12);
        float d02 = DistancePointToEdge(p, x0, x2, n_02);

        d = min(d, d12);
        d = min(d, d02);

        n = (d == d12) ? n_12 : n;
        n = (d == d02) ? n_02 : n;
    }

    d = (dot(p - x0, nTri) < 0.0) ? -d : d;

    return d;
}

void main() {
    int triangleIndex = int(gl_GlobalInvocationID.x);

    uint numTriangleIndices = uint(g_TrimeshVertexIndices.length());
    uint numTriangles = numTriangleIndices / 3u;

    if (triangleIndex >= int(numTriangles)) {
        return;
    }

    uint index0 = g_TrimeshVertexIndices[triangleIndex * 3 + 0];
    uint index1 = g_TrimeshVertexIndices[triangleIndex * 3 + 1];
    uint index2 = g_TrimeshVertexIndices[triangleIndex * 3 + 2];

    vec3 tri0 = collMeshVertexPositions[index0].position.xyz;
    vec3 tri1 = collMeshVertexPositions[index1].position.xyz;
    vec3 tri2 = collMeshVertexPositions[index2].position.xyz;

    float margin = g_CellSize;
    vec3 aabbMin = min(tri0, min(tri1, tri2)) - vec3(margin);
    vec3 aabbMax = max(tri0, max(tri1, tri2)) + vec3(margin);

    ivec3 gridMin = GetSdfCoordinates(aabbMin) - GRID_MARGIN;
    ivec3 gridMax = GetSdfCoordinates(aabbMax) + GRID_MARGIN;

    gridMin.x = clamp(gridMin.x, 0, g_NumCellsX - 1);
    gridMin.y = clamp(gridMin.y, 0, g_NumCellsY - 1);
    gridMin.z = clamp(gridMin.z, 0, g_NumCellsZ - 1);

    gridMax.x = clamp(gridMax.x, 0, g_NumCellsX - 1);
    gridMax.y = clamp(gridMax.y, 0, g_NumCellsY - 1);
    gridMax.z = clamp(gridMax.z, 0, g_NumCellsZ - 1);

    for (int z = gridMin.z; z <= gridMax.z; ++z) {
        for (int y = gridMin.y; y <= gridMax.y; ++y) {
            for (int x = gridMin.x; x <= gridMax.x; ++x) {
                ivec3 gridCellCoordinate = ivec3(x, y, z);
                int gridCellIndex = GetSdfCellIndex(gridCellCoordinate);
                vec3 cellPosition = GetSdfCellPosition(gridCellCoordinate);

                float signedDist = SignedDistancePointToTriangle(cellPosition, tri0, tri1, tri2);
                uint distanceAsUint = FloatFlip3(signedDist);
                atomicMin(g_SignedDistanceField[gridCellIndex], distanceAsUint);
            }
        }
    }
}
