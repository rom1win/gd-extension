#include "HairStrands.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <algorithm>
#include <cstdio>

#include "TressFX/TressFXHairObject.h"
#include "TressFX/TressFXAsset.h"

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
