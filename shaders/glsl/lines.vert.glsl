#version 450

layout(location = 0) in vec3 a_pos;

layout(set = 0, binding = 0) uniform View {
    mat4 u_view_proj;
} view;

void main() {
    gl_Position = view.u_view_proj * vec4(a_pos, 1.0);
}
