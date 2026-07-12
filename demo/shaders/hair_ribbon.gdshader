shader_type spatial;
render_mode cull_disabled, alpha_to_coverage;

// A2.1's output: texel (v % tex_width, v / tex_width) holds simulated hair
// vertex v's position (xyz; w unused). Raw float data, not a color -- do not
// add a source_color hint here.
uniform sampler2D position_tex : filter_nearest;
uniform int tex_width = 512;
uniform int vertices_per_strand = 32;
uniform float fiber_radius = 0.0021;

// A2.3: base/tip color properties (set from TressFXCharacter).
uniform vec3 root_color : source_color = vec3(0.25, 0.12, 0.06);
uniform vec3 tip_color : source_color = vec3(0.55, 0.35, 0.18);

// Kajiya-Kay dual specular lobe constants. Reference:
// thirdparty/tressfx/src/shaders/TressFXLighting.hlsl, ComputeDiffuseSpecFactors.
// Not exposed as node properties -- these tune the "look" of the highlight
// shape, not the hair's color/thickness, and the defaults match AMD's model.
uniform float specular_power_primary = 80.0;
uniform float specular_power_secondary = 12.0;
uniform float specular_strength_primary = 0.4;
uniform float specular_strength_secondary = 0.15;
uniform float cone_angle_degrees = 10.0;

varying float v_fraction_of_strand; // 0 at root, 1 at tip
// light() has no direct access to TANGENT (only vertex()/fragment() do), so
// smuggle the view-space tangent through a varying, set in fragment().
varying vec3 v_tangent_view;

void vertex() {
    // ARRAY_TEX_UV packs the mesh's only per-vertex data: UV.x = global vertex
    // index (an exact integer stored as float), UV.y = ribbon side (0 -> -1, 1 -> +1).
    // The real position is looked up from position_tex below -- VERTEX's input
    // value from the mesh itself is a meaningless placeholder, and so is every
    // other per-vertex attribute (NORMAL/TANGENT/BINORMAL are all recomputed
    // here from scratch).
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
    TANGENT = tangent;
    BINORMAL = right;

    v_fraction_of_strand = float(local) / float(max(vertices_per_strand - 1, 1));
}

void fragment() {
    // UV.y is 0 at one ribbon edge and 1 at the other, smoothly interpolated
    // across the ribbon's width by the rasterizer -- use it to fake a round
    // fiber cross-section instead of a flat card, both for the normal (so the
    // highlight below looks like it's wrapping a cylinder) and for alpha
    // (soft, anti-aliased edges via alpha_to_coverage instead of a hard rectangle).
    float u = UV.y * 2.0 - 1.0; // -1..1 across width, 0 at the strand's own axis
    float cross_section = sqrt(max(0.0, 1.0 - u * u));

    NORMAL = normalize(NORMAL * cross_section + normalize(BINORMAL) * u);
    ALBEDO = mix(root_color, tip_color, v_fraction_of_strand);
    ALPHA = cross_section;

    // TANGENT here is already view-space (Godot transforms it for us); stash
    // it for light() below, which can't read TANGENT directly.
    v_tangent_view = TANGENT;
}

// Kajiya-Kay hair lighting: a diffuse term from Kajiya's original model plus
// two specular lobes (root-shifted "R", tip-shifted "TRT") that make the
// highlight run perpendicular to the strand and shift along its length --
// the signature look of real hair, which a normal PBR material can't produce.
// Reference: thirdparty/tressfx/src/shaders/TressFXLighting.hlsl,
// ComputeDiffuseSpecFactors (this is the same formula; ATTENUATION is Godot's
// own per-light falloff-plus-shadow term, computed by the engine's standard
// pipeline -- not reimplemented here).
void light() {
    vec3 t = normalize(v_tangent_view);

    float cos_tl = clamp(dot(t, LIGHT), -1.0, 1.0);
    float sin_tl = sqrt(max(0.0, 1.0 - cos_tl * cos_tl));

    float cos_te = clamp(dot(t, VIEW), -1.0, 1.0);
    float sin_te = sqrt(max(0.0, 1.0 - cos_te * cos_te));

    float cone = radians(cone_angle_degrees);
    float cos_trl = -cos_tl;
    float sin_trl = sin_tl;

    // Primary highlight, shifted toward the root by 2x the cone angle.
    float cos_trl_root = cos_trl * cos(2.0 * cone) - sin_trl * sin(2.0 * cone);
    float sin_trl_root = sqrt(max(0.0, 1.0 - cos_trl_root * cos_trl_root));
    float spec_root = max(0.0, cos_trl_root * cos_te + sin_trl_root * sin_te);

    // Secondary highlight, shifted toward the tip by 3x the cone angle.
    float cos_trl_tip = cos_trl * cos(-3.0 * cone) - sin_trl * sin(-3.0 * cone);
    float sin_trl_tip = sqrt(max(0.0, 1.0 - cos_trl_tip * cos_trl_tip));
    float spec_tip = max(0.0, cos_trl_tip * cos_te + sin_trl_tip * sin_te);

    // Godot 4: ATTENUATION already includes the shadow term (SHADOW_ATTENUATION
    // was a Godot 3 built-in and no longer exists).
    float atten = ATTENUATION;
    DIFFUSE_LIGHT += ALBEDO * LIGHT_COLOR * sin_tl * atten;
    SPECULAR_LIGHT += LIGHT_COLOR * atten * (
        specular_strength_primary * pow(spec_root, specular_power_primary) +
        specular_strength_secondary * pow(spec_tip, specular_power_secondary));
}
