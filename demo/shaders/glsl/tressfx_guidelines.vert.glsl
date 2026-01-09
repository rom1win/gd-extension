#version 450

layout(location = 0) in vec3 a_dummy;

layout(set = 0, binding = 0, std430) readonly buffer LineVertices {
    vec4 v[];
} verts;

layout(set = 0, binding = 1) uniform View {
    mat4 u_view_proj;
} view;

void main() {
    // a_dummy exists only to satisfy pipeline vertex-input validation.
    // Actual positions are sourced from the storage buffer.
    vec3 p = verts.v[gl_VertexIndex].xyz;
    gl_Position = view.u_view_proj * vec4(p, 1.0);
}
