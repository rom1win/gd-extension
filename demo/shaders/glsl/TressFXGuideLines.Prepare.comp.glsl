#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InPositions {
    vec4 in_pos[];
} input_positions;

layout(set = 0, binding = 1, std430) writeonly buffer OutLineVertices {
    vec4 out_pos[];
} output_vertices;

layout(push_constant) uniform Params {
    uint u_vertices_per_strand;
    uint u_segments;
    uint _pad0;
    uint _pad1;
} params;

void main() {
    uint idx = gl_GlobalInvocationID.x;
    if (idx >= params.u_segments) {
        return;
    }

    uint vps = params.u_vertices_per_strand;
    if (vps < 2u) {
        return;
    }

    uint segments_per_strand = vps - 1u;
    uint strand = idx / segments_per_strand;
    uint seg = idx - strand * segments_per_strand;

    uint base = strand * vps + seg;
    vec4 p0 = input_positions.in_pos[base];
    vec4 p1 = input_positions.in_pos[base + 1u];

    uint out_base = idx * 2u;
    output_vertices.out_pos[out_base + 0u] = p0;
    output_vertices.out_pos[out_base + 1u] = p1;
}
