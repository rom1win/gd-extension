#include "HairStrands.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/array.hpp>
#include <algorithm>
#include <cstdio>

#include "TressFX/TressFXHairObject.h"
#include "TressFX/TressFXAsset.h"
#include "GodotTressFXMath.h"

static godot::String bytes_to_hex_prefix(const godot::PackedByteArray& bytes, int64_t max_bytes) {
    static const char* kHex = "0123456789ABCDEF";
    const int64_t n = std::min<int64_t>(bytes.size(), max_bytes);
    godot::String out;
    for (int64_t i = 0; i < n; ++i) {
        const uint8_t b = bytes[(int)i];
        out += kHex[(b >> 4) & 0xF];
        out += kHex[b & 0xF];
        if (i + 1 < n) {
            out += " ";
        }
    }
    return out;
}

static void log_file_probe(const char* label, const std::string& path_utf8) {
    using namespace godot;

    const String path = String(path_utf8.c_str());
    String global = path;
    if (ProjectSettings::get_singleton()) {
        global = ProjectSettings::get_singleton()->globalize_path(path);
    }

    // Prefer opening the original path (res://) since it works in both editor and exported builds.
    Ref<FileAccess> f;
    if (FileAccess::file_exists(path)) {
        f = FileAccess::open(path, FileAccess::READ);
    }
    if (!f.is_valid() && FileAccess::file_exists(global)) {
        f = FileAccess::open(global, FileAccess::READ);
    }

    if (!f.is_valid()) {
        UtilityFunctions::print(
            String("HairStrands: ") + label +
            String(" open FAILED path='") + path +
            String("' global='") + global +
            String("' open_error=") + String::num_int64((int)FileAccess::get_open_error()));
        return;
    }

    const uint64_t len = f->get_length();
    const int64_t probe_len = (int64_t)std::min<uint64_t>(len, 16);
    f->seek(0);
    const PackedByteArray head = f->get_buffer(probe_len);

    UtilityFunctions::print(
        String("HairStrands: ") + label +
        String(" opened ok path='") + path +
        String("' global='") + global +
        String("' bytes=") + String::num_int64((int64_t)len) +
        String(" head[") + String::num_int64(probe_len) + String("]=") + bytes_to_hex_prefix(head, 16));
}

static FILE* open_file_for_tressfx(const std::string& path_utf8) {
    using namespace godot;

    const String path = String(path_utf8.c_str());
    String global = path;
    if (ProjectSettings::get_singleton()) {
        global = ProjectSettings::get_singleton()->globalize_path(path);
    }

    // 1) Try stdio open on the globalized path (fast path; works in editor).
    {
        const CharString global_utf8 = global.utf8();
        const char* global_cstr = global_utf8.get_data();
        FILE* fp = nullptr;
#if defined(_MSC_VER)
        if (global_cstr && global_cstr[0] != '\0') {
            fopen_s(&fp, global_cstr, "rb");
        }
#else
        if (global_cstr && global_cstr[0] != '\0') {
            fp = std::fopen(global_cstr, "rb");
        }
#endif
        if (fp) {
            return fp;
        }
    }

    // 2) Fallback: read via Godot VFS (res:// works in exports), then spool into a temp FILE*.
    Ref<FileAccess> f;
    if (FileAccess::file_exists(path)) {
        f = FileAccess::open(path, FileAccess::READ);
    }
    if (!f.is_valid() && FileAccess::file_exists(global)) {
        f = FileAccess::open(global, FileAccess::READ);
    }
    if (!f.is_valid()) {
        return nullptr;
    }

    const uint64_t len = f->get_length();
    f->seek(0);
    const PackedByteArray bytes = f->get_buffer((int64_t)len);

    FILE* tmp = std::tmpfile();
    if (!tmp) {
        return nullptr;
    }

    const uint8_t* data = bytes.ptr();
    if (data && bytes.size() > 0) {
        std::fwrite(data, 1, (size_t)bytes.size(), tmp);
    }
    std::fseek(tmp, 0, SEEK_SET);
    return tmp;
}

HairStrands::~HairStrands() = default;

HairStrands::HairStrands(
    EI_Scene* scene,
    const char* tfxFilePath,
    const char* tfxboneFilePath,
    const char* hairObjectName,
    int numFollowHairsPerGuideHair,
    float tipSeparationFactor,
    int skinNumber,
    int renderIndex)
    : m_pScene(scene),
      m_skinNumber(skinNumber),
      m_tfxFilePath(tfxFilePath ? tfxFilePath : ""),
      m_tfxboneFilePath(tfxboneFilePath ? tfxboneFilePath : ""),
      m_hairObjectName(hairObjectName ? hairObjectName : ""),
      m_numFollowHairsPerGuideHair(numFollowHairsPerGuideHair),
      m_tipSeparationFactor(tipSeparationFactor),
      m_renderIndex(renderIndex) {
    // First incremental implementation:
    // Do not create TressFXHairObject yet (it needs a real EI_Device and command context).
    // This keeps runtime safe while we bring up the rendering backend.

        godot::UtilityFunctions::print(
                godot::String("HairStrands: constructed renderIndex=") + godot::String::num_int64(m_renderIndex) +
                godot::String(" skin=") + godot::String::num_int64(m_skinNumber) +
                godot::String(" tfx='") + godot::String(m_tfxFilePath.c_str()) +
                godot::String("' bone='") + godot::String(m_tfxboneFilePath.c_str()) +
                godot::String("' obj='") + godot::String(m_hairObjectName.c_str()) +
                godot::String("'"));

            // Step 0: prove we can read the assets the user configured.
            // (This does not parse the formats yet; it just proves file access and logs a small header.)
            log_file_probe("tfx", m_tfxFilePath);
            log_file_probe("tfxbone", m_tfxboneFilePath);

            // Step 1: actually load the asset data via TressFXAsset (CPU-side only).
            // This does not require EI_Device or any GPU resources.
            m_asset = std::make_unique<TressFXAsset>();

            FILE* hair_fp = open_file_for_tressfx(m_tfxFilePath);
            if (!hair_fp) {
                godot::UtilityFunctions::print(
                    godot::String("HairStrands: TressFXAsset hair open FAILED: '") + godot::String(m_tfxFilePath.c_str()) + godot::String("'"));
                m_asset.reset();
                return;
            }

            const bool hair_ok = m_asset->LoadHairData(hair_fp);
            std::fclose(hair_fp);
            if (!hair_ok) {
                godot::UtilityFunctions::print(
                    godot::String("HairStrands: TressFXAsset::LoadHairData FAILED for '") + godot::String(m_tfxFilePath.c_str()) + godot::String("'"));
                m_asset.reset();
                return;
            }

            // Optional follow hairs.
            m_asset->GenerateFollowHairs(m_numFollowHairsPerGuideHair, m_tipSeparationFactor, /*maxRadiusAroundGuideHair=*/0.0f);

            if (!m_asset->ProcessAsset()) {
                godot::UtilityFunctions::print(
                    godot::String("HairStrands: TressFXAsset::ProcessAsset FAILED for '") + godot::String(m_tfxFilePath.c_str()) + godot::String("'"));
                m_asset.reset();
                return;
            }

            // Bone data is optional but usually required for skinned characters.
            if (!m_tfxboneFilePath.empty() && m_pScene) {
                FILE* bone_fp = open_file_for_tressfx(m_tfxboneFilePath);
                if (!bone_fp) {
                    godot::UtilityFunctions::print(
                        godot::String("HairStrands: TressFXAsset bone open FAILED: '") + godot::String(m_tfxboneFilePath.c_str()) + godot::String("'"));
                } else {
                    const bool bone_ok = m_asset->LoadBoneData(bone_fp, m_skinNumber, m_pScene);
                    std::fclose(bone_fp);
                    if (!bone_ok) {
                        godot::UtilityFunctions::print(
                            godot::String("HairStrands: TressFXAsset::LoadBoneData FAILED for '") + godot::String(m_tfxboneFilePath.c_str()) + godot::String("'"));
                    }
                }
            }

            godot::UtilityFunctions::print(
                godot::String("HairStrands: asset loaded ok strands=") + godot::String::num_int64(m_asset->m_numTotalStrands) +
                godot::String(" verts=") + godot::String::num_int64(m_asset->m_numTotalVertices) +
                godot::String(" vps=") + godot::String::num_int64(m_asset->m_numVerticesPerStrand) +
                godot::String(" guides=") + godot::String::num_int64(m_asset->m_numGuideStrands) +
                godot::String(" followPerGuide=") + godot::String::num_int64(m_asset->m_numFollowStrandsPerGuide));
}

void HairStrands::TransitionSimToRendering(EI_CommandContext& context) {
    if (!m_pStrands) {
        // No GPU resources yet.
        return;
    }
    m_pStrands->GetDynamicState().TransitionSimToRendering(context);
}

void HairStrands::TransitionRenderingToSim(EI_CommandContext& context) {
    if (!m_pStrands) {
        // No GPU resources yet.
        return;
    }
    m_pStrands->GetDynamicState().TransitionRenderingToSim(context);
}

void HairStrands::UpdateBones(EI_CommandContext& context) {
    (void)context;
    // Intentionally no-op in this incremental step.
    // Once EI_Scene exposes bone matrices in a TressFX-friendly format, we will:
    // - query skeleton matrices
    // - call m_pStrands->UpdateBoneMatrices(...)
}

godot::Ref<godot::ArrayMesh> HairStrands::CreateDebugLineMesh(bool guides_only, int strand_limit) const {
    if (!m_asset) {
        return godot::Ref<godot::ArrayMesh>();
    }

    const int vps = (int)m_asset->m_numVerticesPerStrand;
    if (vps < 2) {
        return godot::Ref<godot::ArrayMesh>();
    }

    const int total_strands = guides_only ? (int)m_asset->m_numGuideStrands : (int)m_asset->m_numTotalStrands;
    int strands_to_draw = total_strands;
    if (strand_limit > 0 && strand_limit < strands_to_draw) {
        strands_to_draw = strand_limit;
    }

    const int follow_per_guide = (int)m_asset->m_numFollowStrandsPerGuide;
    const int guide_stride = follow_per_guide + 1;

    // Each strand has (vps-1) segments; each segment contributes 2 vertices for line rendering.
    const int segments_per_strand = vps - 1;
    const int64_t total_vertices = (int64_t)strands_to_draw * (int64_t)segments_per_strand * 2;
    if (total_vertices <= 0) {
        return godot::Ref<godot::ArrayMesh>();
    }

    godot::PackedVector3Array verts;
    verts.resize(total_vertices);

    int64_t out_i = 0;
    for (int s = 0; s < strands_to_draw; ++s) {
        const int strand_index = guides_only ? (s * guide_stride) : s;
        const int base = strand_index * vps;
        for (int v = 0; v < vps - 1; ++v) {
            const godot::Vector3 p0(m_asset->m_positions[base + v].x, m_asset->m_positions[base + v].y, m_asset->m_positions[base + v].z);
            const godot::Vector3 p1(m_asset->m_positions[base + v + 1].x, m_asset->m_positions[base + v + 1].y, m_asset->m_positions[base + v + 1].z);
            verts[(int)out_i++] = p0;
            verts[(int)out_i++] = p1;
        }
    }

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = verts;

    godot::Ref<godot::ArrayMesh> mesh;
    mesh.instantiate();
    mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_LINES, arrays);
    return mesh;
}

godot::Ref<godot::ArrayMesh> HairStrands::CreateDebugSkinnedLineMesh(bool guides_only, int strand_limit) const {
    if (!m_asset || !m_pScene) {
        return godot::Ref<godot::ArrayMesh>();
    }

    if (m_asset->m_boneSkinningData.empty()) {
        return godot::Ref<godot::ArrayMesh>();
    }

    const std::vector<XMMATRIX>& bone_mats = m_pScene->GetWorldSpaceSkeletonMats(m_skinNumber);
    if (bone_mats.empty()) {
        return godot::Ref<godot::ArrayMesh>();
    }

    const int vps = (int)m_asset->m_numVerticesPerStrand;
    if (vps < 2) {
        return godot::Ref<godot::ArrayMesh>();
    }

    const int total_strands = guides_only ? (int)m_asset->m_numGuideStrands : (int)m_asset->m_numTotalStrands;
    int strands_to_draw = total_strands;
    if (strand_limit > 0 && strand_limit < strands_to_draw) {
        strands_to_draw = strand_limit;
    }

    const int follow_per_guide = (int)m_asset->m_numFollowStrandsPerGuide;
    const int guide_stride = follow_per_guide + 1;

    const int segments_per_strand = vps - 1;
    const int64_t total_vertices = (int64_t)strands_to_draw * (int64_t)segments_per_strand * 2;
    if (total_vertices <= 0) {
        return godot::Ref<godot::ArrayMesh>();
    }

    godot::PackedVector3Array verts;
    verts.resize(total_vertices);

    int64_t out_i = 0;
    for (int s = 0; s < strands_to_draw; ++s) {
        const int strand_index = guides_only ? (s * guide_stride) : s;
        const int base = strand_index * vps;
        if (base + (vps - 1) >= (int)m_asset->m_positions.size()) {
            break;
        }

        const TressFXBoneSkinningData& skin = m_asset->m_boneSkinningData[std::min<int>(strand_index, (int)m_asset->m_boneSkinningData.size() - 1)];

        // Interpolate bone matrices using weights (matches TressFXBoneSkinning.hlsl convention).
        XMMATRIX bone_matrix;
        float weight_sum = 0.0f;
        for (int i = 0; i < TRESSFX_MAX_INFLUENTIAL_BONE_COUNT; ++i) {
            const float w = skin.weight[i];
            if (w <= 0.0f) {
                continue;
            }
            int bi = (int)skin.boneIndex[i];
            if (bi < 0) {
                bi = 0;
            }
            if (bi >= (int)bone_mats.size()) {
                bi = 0;
            }
            bone_matrix += bone_mats[bi] * w;
            weight_sum += w;
        }
        if (weight_sum > 0.0f) {
            bone_matrix /= weight_sum;
        }

        auto transform_pos = [&](const Vector3& p) -> godot::Vector3 {
            const XMVECTOR v{ p.x, p.y, p.z, 1.0f };
            const XMVECTOR r = XMVector4Transform(v, bone_matrix);
            return godot::Vector3(r.x, r.y, r.z);
        };

        for (int v = 0; v < vps - 1; ++v) {
            const Vector3& p0_raw = m_asset->m_positions[base + v];
            const Vector3& p1_raw = m_asset->m_positions[base + v + 1];
            const godot::Vector3 p0 = transform_pos(p0_raw);
            const godot::Vector3 p1 = transform_pos(p1_raw);
            verts[(int)out_i++] = p0;
            verts[(int)out_i++] = p1;
        }
    }

    if (out_i != total_vertices) {
        verts.resize(out_i);
    }

    godot::Array arrays;
    arrays.resize(godot::Mesh::ARRAY_MAX);
    arrays[godot::Mesh::ARRAY_VERTEX] = verts;

    godot::Ref<godot::ArrayMesh> mesh;
    mesh.instantiate();
    mesh->add_surface_from_arrays(godot::Mesh::PRIMITIVE_LINES, arrays);
    return mesh;
}

int HairStrands::GetGuideStrandCount() const {
    if (!m_asset) {
        return 0;
    }
    return (int)m_asset->m_numGuideStrands;
}

bool HairStrands::PackGuidePositionsVec4(godot::PackedByteArray& out_bytes, int& out_vertices_per_strand, int& out_guide_strands) const {
    out_bytes = godot::PackedByteArray();
    out_vertices_per_strand = 0;
    out_guide_strands = 0;

    if (!m_asset) {
        return false;
    }

    const int vps = (int)m_asset->m_numVerticesPerStrand;
    const int guide_strands = (int)m_asset->m_numGuideStrands;
    if (vps <= 0 || guide_strands <= 0) {
        return false;
    }

    const int follow_per_guide = (int)m_asset->m_numFollowStrandsPerGuide;
    const int guide_stride = follow_per_guide + 1;

    const std::vector<XMMATRIX>& bone_mats = (m_pScene ? m_pScene->GetWorldSpaceSkeletonMats(m_skinNumber) : std::vector<XMMATRIX>{});
    const bool can_skin = (!bone_mats.empty() && !m_asset->m_boneSkinningData.empty());

    const int total_positions = guide_strands * vps;
    const int bytes_per_pos = 16; // vec4
    const int total_bytes = total_positions * bytes_per_pos;
    out_bytes.resize(total_bytes);
    uint8_t* dst_u8 = out_bytes.ptrw();

    float* dst = reinterpret_cast<float*>(dst_u8);
    int out_i = 0;
    for (int g = 0; g < guide_strands; ++g) {
        const int strand_index = g * guide_stride;
        const int base = strand_index * vps;

        XMMATRIX bone_matrix;
        if (can_skin) {
            const int skin_index = std::min<int>(strand_index, (int)m_asset->m_boneSkinningData.size() - 1);
            const TressFXBoneSkinningData& skin = m_asset->m_boneSkinningData[skin_index];
            float weight_sum = 0.0f;
            for (int i = 0; i < TRESSFX_MAX_INFLUENTIAL_BONE_COUNT; ++i) {
                const float w = skin.weight[i];
                if (w <= 0.0f) {
                    continue;
                }
                int bi = (int)skin.boneIndex[i];
                if (bi < 0) {
                    bi = 0;
                }
                if (bi >= (int)bone_mats.size()) {
                    bi = 0;
                }
                bone_matrix += bone_mats[bi] * w;
                weight_sum += w;
            }
            if (weight_sum > 0.0f) {
                bone_matrix /= weight_sum;
            }
        }

        for (int v = 0; v < vps; ++v) {
            const Vector3& p = m_asset->m_positions[base + v];
            if (can_skin) {
                const XMVECTOR vin{ p.x, p.y, p.z, 1.0f };
                const XMVECTOR r = XMVector4Transform(vin, bone_matrix);
                dst[out_i * 4 + 0] = r.x;
                dst[out_i * 4 + 1] = r.y;
                dst[out_i * 4 + 2] = r.z;
                dst[out_i * 4 + 3] = 1.0f;
            } else {
                dst[out_i * 4 + 0] = p.x;
                dst[out_i * 4 + 1] = p.y;
                dst[out_i * 4 + 2] = p.z;
                dst[out_i * 4 + 3] = 1.0f;
            }
            out_i++;
        }
    }

    out_vertices_per_strand = vps;
    out_guide_strands = guide_strands;
    return true;
}

int HairStrands::GetTotalStrandCount() const {
    if (!m_asset) {
        return 0;
    }
    return (int)m_asset->m_numTotalStrands;
}
