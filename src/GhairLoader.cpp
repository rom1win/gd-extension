#include "GhairLoader.h"

#include "TressFX/TressFXAsset.h"
#include "TressFX/TressFXCommon.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

const char kMagic[8] = { 'G', 'H', 'A', 'I', 'R', 'v', '0', '1' };

bool read_exact(FILE* f, void* dst, size_t n) {
    return n == 0 || std::fread(dst, 1, n, f) == n;
}

void warn(const char* path, const godot::String& msg) {
    godot::UtilityFunctions::push_warning(
        godot::String("GhairLoader: '") + godot::String(path ? path : "") + godot::String("': ") + msg);
}

} // namespace

bool GhairLoader::LoadHairData(FILE* file, const char* debug_path, TressFXAsset* asset, int* out_raw_num_strands) {
    if (out_raw_num_strands) {
        *out_raw_num_strands = 0;
    }
    if (!file || !asset) {
        return false;
    }

    std::fseek(file, 0, SEEK_SET);

    char magic[8];
    uint32_t numStrandsInFile = 0, vps = 0, numChunks = 0;
    if (!read_exact(file, magic, sizeof(magic)) ||
        !read_exact(file, &numStrandsInFile, sizeof(numStrandsInFile)) ||
        !read_exact(file, &vps, sizeof(vps)) ||
        !read_exact(file, &numChunks, sizeof(numChunks))) {
        warn(debug_path, "truncated header");
        return false;
    }
    if (std::memcmp(magic, kMagic, sizeof(kMagic)) != 0) {
        warn(debug_path, "bad magic");
        return false;
    }
    if (numStrandsInFile == 0) {
        warn(debug_path, "numStrands is 0");
        return false;
    }
    // Same constraint TressFXAsset::LoadHairData asserts for .tfx: vertices
    // per strand must be >2, <=TRESSFX_SIM_THREAD_GROUP_SIZE, and evenly
    // divide it (4/8/16/32/64 are the only valid values).
    if (vps <= 2 || vps > TRESSFX_SIM_THREAD_GROUP_SIZE || (TRESSFX_SIM_THREAD_GROUP_SIZE % vps) != 0) {
        warn(debug_path, godot::String("invalid vps=") + godot::String::num_int64((int64_t)vps));
        return false;
    }

    std::vector<float> pos_floats;
    std::vector<float> uv_floats;
    bool have_pos = false, have_uv = false;

    for (uint32_t c = 0; c < numChunks; ++c) {
        char fourcc[4];
        uint64_t payload_len = 0;
        if (!read_exact(file, fourcc, sizeof(fourcc)) || !read_exact(file, &payload_len, sizeof(payload_len))) {
            warn(debug_path, "truncated chunk header");
            return false;
        }

        if (std::memcmp(fourcc, "POS0", 4) == 0) {
            const uint64_t expected = (uint64_t)numStrandsInFile * vps * 3 * sizeof(float);
            if (payload_len != expected) {
                warn(debug_path, "POS0 chunk size mismatch");
                return false;
            }
            pos_floats.resize((size_t)numStrandsInFile * vps * 3);
            if (!read_exact(file, pos_floats.data(), (size_t)payload_len)) {
                warn(debug_path, "truncated POS0 payload");
                return false;
            }
            have_pos = true;
        } else if (std::memcmp(fourcc, "RTUV", 4) == 0) {
            const uint64_t expected = (uint64_t)numStrandsInFile * 2 * sizeof(float);
            if (payload_len != expected) {
                warn(debug_path, "RTUV chunk size mismatch");
                return false;
            }
            uv_floats.resize((size_t)numStrandsInFile * 2);
            if (!read_exact(file, uv_floats.data(), (size_t)payload_len)) {
                warn(debug_path, "truncated RTUV payload");
                return false;
            }
            have_uv = true;
        } else {
            // Unknown/unneeded chunk (META, WID0, TWST, ...) -- readers must skip.
            if (std::fseek(file, (long)payload_len, SEEK_CUR) != 0) {
                warn(debug_path, "truncated chunk payload");
                return false;
            }
        }
    }

    if (!have_pos) {
        warn(debug_path, "missing required chunk POS0");
        return false;
    }
    if (!have_uv) {
        warn(debug_path, "missing required chunk RTUV");
        return false;
    }

    // Pad strand count up to a multiple of the sim thread-group size --
    // identical formula to TressFXAsset::LoadHairData's .tfx path (always
    // rounds up to the NEXT multiple, even when numStrandsInFile already is
    // one; that is AMD's own behavior, replicated verbatim here).
    asset->m_numGuideStrands = (numStrandsInFile - numStrandsInFile % TRESSFX_SIM_THREAD_GROUP_SIZE) + TRESSFX_SIM_THREAD_GROUP_SIZE;
    asset->m_numVerticesPerStrand = (AMD::int32)vps;
    asset->m_numFollowStrandsPerGuide = 0;
    asset->m_numTotalStrands = asset->m_numGuideStrands;
    asset->m_numGuideVertices = asset->m_numGuideStrands * asset->m_numVerticesPerStrand;
    asset->m_numTotalVertices = asset->m_numGuideVertices;

    asset->m_positions.resize(asset->m_numTotalVertices);
    for (uint32_t s = 0; s < numStrandsInFile; ++s) {
        for (uint32_t v = 0; v < vps; ++v) {
            const size_t src = ((size_t)s * vps + v) * 3;
            Vector3& p = asset->m_positions[(size_t)s * vps + v];
            p.x = pos_floats[src + 0];
            p.y = pos_floats[src + 1];
            p.z = pos_floats[src + 2];
            // Movability convention (w component): first two vertices of each
            // strand pinned (0 = not movable), remaining vertices free (1).
            p.w = (v < 2) ? 0.0f : 1.0f;
        }
    }
    // Pad by duplicating the last real strand's data, same as .tfx.
    const uint32_t numStrandsToMakeUp = asset->m_numGuideStrands - numStrandsInFile;
    for (uint32_t i = 0; i < numStrandsToMakeUp; ++i) {
        for (uint32_t v = 0; v < vps; ++v) {
            const size_t lastIdx = ((size_t)numStrandsInFile - 1) * vps + v;
            const size_t dstIdx = ((size_t)numStrandsInFile + i) * vps + v;
            asset->m_positions[dstIdx] = asset->m_positions[lastIdx];
        }
    }

    asset->m_strandUV.resize(asset->m_numTotalStrands);
    for (uint32_t s = 0; s < numStrandsInFile; ++s) {
        asset->m_strandUV[s].x = uv_floats[(size_t)s * 2 + 0];
        asset->m_strandUV[s].y = uv_floats[(size_t)s * 2 + 1];
    }
    for (uint32_t i = 0; i < numStrandsToMakeUp; ++i) {
        asset->m_strandUV[numStrandsInFile + i] = asset->m_strandUV[numStrandsInFile - 1];
    }

    asset->m_followRootOffsets.resize(asset->m_numTotalStrands);
    std::memset(asset->m_followRootOffsets.data(), 0, asset->m_numTotalStrands * sizeof(AMD::float4));

    if (out_raw_num_strands) {
        *out_raw_num_strands = (int)numStrandsInFile;
    }
    return true;
}

void GhairLoader::FillUniformBoneSkinning(TressFXAsset* asset) {
    if (!asset) {
        return;
    }
    TressFXBoneSkinningData skin;
    skin.boneIndex[0] = 0.0f;
    skin.boneIndex[1] = -1.0f;
    skin.boneIndex[2] = -1.0f;
    skin.boneIndex[3] = -1.0f;
    skin.weight[0] = 1.0f;
    skin.weight[1] = 0.0f;
    skin.weight[2] = 0.0f;
    skin.weight[3] = 0.0f;
    asset->m_boneSkinningData.assign(asset->m_numTotalStrands, skin);
}

void GhairLoader::FillBoundBoneSkinning(TressFXAsset* asset, const std::vector<TressFXBoneSkinningData>& perGuideSkinning) {
    if (!asset) {
        return;
    }
    const int guideStride = asset->m_numFollowStrandsPerGuide + 1;
    const int numGuides = asset->m_numGuideStrands;
    if ((int)perGuideSkinning.size() != numGuides) {
        warn(nullptr, godot::String("FillBoundBoneSkinning: perGuideSkinning size mismatch (got ") +
            godot::String::num_int64((int64_t)perGuideSkinning.size()) + godot::String(", expected ") +
            godot::String::num_int64(numGuides) + godot::String(")"));
        return;
    }

    asset->m_boneSkinningData.assign(asset->m_numTotalStrands, TressFXBoneSkinningData{});
    for (int g = 0; g < numGuides; ++g) {
        const int base = g * guideStride;
        for (int f = 0; f < guideStride; ++f) {
            asset->m_boneSkinningData[base + f] = perGuideSkinning[g];
        }
    }
}
