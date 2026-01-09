#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, r32ui) uniform writeonly uimage2D img;

void main() {
    imageStore(img, ivec2(0, 0), uvec4(789u, 0u, 0u, 0u));
}
