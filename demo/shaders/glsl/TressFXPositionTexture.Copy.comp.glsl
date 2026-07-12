#version 450

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer InPositions {
    vec4 in_pos[];
} input_positions;

layout(set = 0, binding = 1, rgba32f) uniform writeonly image2D out_tex;

layout(push_constant) uniform Params {
    uint u_vertex_count;
    uint u_tex_width;
    uint _pad0;
    uint _pad1;
} params;

// A2.1: copy every simulated hair vertex position (float4, xyz used) into an
// RGBA32F texture so a Godot spatial shader can texelFetch() it without any
// CPU readback. Vertex v lives at texel (v % tex_width, v / tex_width).
void main() {
    uint v = gl_GlobalInvocationID.x;
    if (v >= params.u_vertex_count) {
        return;
    }
    ivec2 texel = ivec2(int(v % params.u_tex_width), int(v / params.u_tex_width));
    imageStore(out_tex, texel, input_positions.in_pos[v]);
}
