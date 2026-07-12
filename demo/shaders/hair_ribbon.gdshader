shader_type spatial;
render_mode cull_disabled, unshaded;

// A2.1's output: texel (v % tex_width, v / tex_width) holds simulated hair
// vertex v's position (xyz; w unused). Raw float data, not a color -- do not
// add a source_color hint here.
uniform sampler2D position_tex : filter_nearest;
uniform int tex_width = 512;
uniform int vertices_per_strand = 32;
uniform float fiber_radius = 0.0021;

void vertex() {
    // ARRAY_TEX_UV packs the mesh's only per-vertex data: UV.x = global vertex
    // index (an exact integer stored as float), UV.y = ribbon side (0 -> -1, 1 -> +1).
    // The real position is looked up from position_tex below -- VERTEX's input
    // value from the mesh itself is a meaningless placeholder.
    int vid = int(UV.x + 0.5);
    float side = (UV.y > 0.5) ? 1.0 : -1.0;

    int local = vid % vertices_per_strand;
    bool has_next = local < (vertices_per_strand - 1);
    int neighbor_vid = has_next ? (vid + 1) : (vid - 1);

    ivec2 texel_cur = ivec2(vid % tex_width, vid / tex_width);
    ivec2 texel_nbr = ivec2(neighbor_vid % tex_width, neighbor_vid / tex_width);

    vec3 pos_cur = texelFetch(position_tex, texel_cur, 0).xyz;
    vec3 pos_nbr = texelFetch(position_tex, texel_nbr, 0).xyz;
    vec3 tangent = has_next ? normalize(pos_nbr - pos_cur) : normalize(pos_cur - pos_nbr);

    // Camera position in this mesh's local (object) space. Assumes MODEL_MATRIX
    // is rigid (rotation + translation, no scale) -- true for the bone-follow
    // debug root this mesh is parented under (see refresh_gpu_debug_hair_lines_3d).
    vec3 model_translation = MODEL_MATRIX[3].xyz;
    mat3 model_rotation = mat3(MODEL_MATRIX);
    vec3 eye_local = transpose(model_rotation) * (CAMERA_POSITION_WORLD - model_translation);

    vec3 view_dir = normalize(pos_cur - eye_local);
    vec3 right = cross(tangent, view_dir);
    float right_len = length(right);
    // Degenerate when the view direction lines up with the strand (looking
    // straight down it) -- fall back to a world-up-based right vector.
    right = (right_len > 0.0001) ? (right / right_len) : normalize(cross(tangent, vec3(0.0, 1.0, 0.0)));

    VERTEX = pos_cur + right * side * fiber_radius;
    NORMAL = normalize(cross(right, tangent));
}

void fragment() {
    // Placeholder color; real shading (base/tip color, anisotropic highlight,
    // alpha-to-coverage) is A2.3.
    ALBEDO = vec3(0.75, 0.55, 0.35);
}
