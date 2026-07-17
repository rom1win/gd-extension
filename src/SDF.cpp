#include "SDF.h"

#include "EngineInterface.h" // EI_Device, EI_Resource, EI_CommandContext, EI_BF_*, EI_Scene, GetDevice()

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Mirrors HairStrands.cpp's open_file_for_tressfx: fast stdio path via the
// globalized filesystem path (works in-editor), falling back to Godot's VFS
// (res:// in exported builds) spooled into a temp FILE*.
FILE* open_file_for_collision_mesh(const std::string& path_utf8) {
    using namespace godot;

    const String path = String(path_utf8.c_str());
    String global = path;
    if (ProjectSettings::get_singleton()) {
        global = ProjectSettings::get_singleton()->globalize_path(path);
    }

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

// Line-based reader (the .tfxmesh format is AMD's plain-text asset format,
// unlike the binary .tfx/.tfxbone), mirroring the getline() loop in
// TressFXBoneSkinning::LoadTressFXCollisionMeshData but over a FILE* so it
// can be fed by either the fast stdio path or the VFS fallback above.
bool read_line(FILE* fp, std::string& out) {
    out.clear();
    char buf[512];
    bool any = false;
    while (std::fgets(buf, sizeof(buf), fp)) {
        any = true;
        out += buf;
        const size_t n = out.size();
        if (n > 0 && out[n - 1] == '\n') {
            if (n > 1 && out[n - 2] == '\r') {
                out.erase(n - 2);
            } else {
                out.erase(n - 1);
            }
            return true;
        }
    }
    return any;
}

int tokenize(const std::string& line, std::vector<std::string>& tokens) {
    tokens.clear();
    size_t i = 0;
    const size_t n = line.size();
    while (i < n) {
        while (i < n && line[i] == ' ') {
            ++i;
        }
        const size_t start = i;
        while (i < n && line[i] != ' ') {
            ++i;
        }
        if (i > start) {
            tokens.push_back(line.substr(start, i - start));
        }
    }
    return (int)tokens.size();
}

} // namespace

CollisionMesh::~CollisionMesh() = default;

CollisionMesh::CollisionMesh(
    EI_Scene* scene,
    EI_RenderTargetSet* renderPass,
    const char* name,
    const char* tfxmeshFilePath,
    int numCellsInXAxis,
    float SDFCollMargin,
    int skinNumber,
    const char* followBone)
    : m_pScene(scene),
      m_name(name ? name : ""),
      m_tfxmeshFilePath(tfxmeshFilePath ? tfxmeshFilePath : ""),
      m_followBone(followBone ? followBone : ""),
      m_numCellsInXAxis(numCellsInXAxis),
      m_SDFCollMargin(SDFCollMargin),
      m_skinNumber(skinNumber) {
    (void)renderPass;

    godot::UtilityFunctions::print(
            godot::String("CollisionMesh: constructed name='") + godot::String(m_name.c_str()) +
            godot::String("' tfxmesh='") + godot::String(m_tfxmeshFilePath.c_str()) +
            godot::String("' followBone='") + godot::String(m_followBone.c_str()) +
            godot::String("' cells=") + godot::String::num_int64(m_numCellsInXAxis) +
            godot::String(" margin=") + godot::String::num(m_SDFCollMargin, 3));

    if (!LoadTfxMesh()) {
        godot::UtilityFunctions::push_warning(
                godot::String("CollisionMesh: failed to load '") + godot::String(m_tfxmeshFilePath.c_str()) +
                godot::String("' -- collision mesh disabled, character keeps running without it"));
        m_valid = false;
        return;
    }

    m_valid = true;
    godot::UtilityFunctions::print(
            godot::String("CollisionMesh: loaded '") + godot::String(m_tfxmeshFilePath.c_str()) +
            godot::String("' bones=") + godot::String::num_int64(m_numBonesInFile) +
            godot::String(" verts=") + godot::String::num_int64(m_numVertices) +
            godot::String(" tris=") + godot::String::num_int64(m_numTriangles) +
            godot::String(" aabb_min=(") + godot::String::num(m_aabbMin.x, 3) + godot::String(",") +
            godot::String::num(m_aabbMin.y, 3) + godot::String(",") + godot::String::num(m_aabbMin.z, 3) +
            godot::String(") aabb_max=(") + godot::String::num(m_aabbMax.x, 3) + godot::String(",") +
            godot::String::num(m_aabbMax.y, 3) + godot::String(",") + godot::String::num(m_aabbMax.z, 3) +
            godot::String(")"));
}

bool CollisionMesh::LoadTfxMesh() {
    FILE* fp = open_file_for_collision_mesh(m_tfxmeshFilePath);
    if (!fp) {
        godot::UtilityFunctions::print(
                godot::String("CollisionMesh: open FAILED: '") + godot::String(m_tfxmeshFilePath.c_str()) + godot::String("'"));
        return false;
    }

    std::vector<std::string> boneNames;
    std::string line;
    std::vector<std::string> tokens;
    int numOfVertices = 0;
    int numOfTriangles = 0;

    while (read_line(fp, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        tokenize(line, tokens);
        if (tokens.empty()) {
            continue;
        }

        if (tokens[0] == "numOfBones") {
            const int numOfBones = tokens.size() > 1 ? std::atoi(tokens[1].c_str()) : 0;
            int count = 0;
            while (count < numOfBones && read_line(fp, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                tokenize(line, tokens);
                if (tokens.size() < 2) {
                    continue;
                }
                boneNames.push_back(tokens[1]);
                ++count;
            }
            m_numBonesInFile = (int)boneNames.size();
        } else if (tokens[0] == "numOfVertices") {
            numOfVertices = tokens.size() > 1 ? std::atoi(tokens[1].c_str()) : 0;
            if (numOfVertices <= 0) {
                std::fclose(fp);
                return false;
            }
            m_positions.assign(numOfVertices, godot::Vector3());
            m_normals.assign(numOfVertices, godot::Vector3());
            m_boneData.assign(numOfVertices, VertexBoneData());

            int index = 0;
            while (index < numOfVertices && read_line(fp, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                tokenize(line, tokens);
                if ((int)tokens.size() < 15) {
                    continue;
                }

                godot::Vector3& pos = m_positions[index];
                pos.x = (float)std::atof(tokens[1].c_str());
                pos.y = (float)std::atof(tokens[2].c_str());
                pos.z = (float)std::atof(tokens[3].c_str());

                godot::Vector3& nrm = m_normals[index];
                nrm.x = (float)std::atof(tokens[4].c_str());
                nrm.y = (float)std::atof(tokens[5].c_str());
                nrm.z = (float)std::atof(tokens[6].c_str());

                VertexBoneData& bd = m_boneData[index];
                for (int k = 0; k < 4; ++k) {
                    const int fileBoneIdx = std::atoi(tokens[7 + k].c_str());
                    int engineBoneIdx = 0;
                    if (m_pScene && fileBoneIdx >= 0 && fileBoneIdx < (int)boneNames.size()) {
                        engineBoneIdx = m_pScene->GetBoneIdByName(m_skinNumber, boneNames[fileBoneIdx].c_str());
                    }
                    bd.boneIndex[k] = (float)engineBoneIdx;
                    bd.weight[k] = (float)std::atof(tokens[11 + k].c_str());
                }

                ++index;
            }
            if (index != numOfVertices) {
                std::fclose(fp);
                return false;
            }
        } else if (tokens[0] == "numOfTriangles") {
            numOfTriangles = tokens.size() > 1 ? std::atoi(tokens[1].c_str()) : 0;
            if (numOfTriangles <= 0) {
                std::fclose(fp);
                return false;
            }
            m_indices.assign((size_t)numOfTriangles * 3, 0);

            int index = 0;
            while (index < numOfTriangles && read_line(fp, line)) {
                if (line.empty() || line[0] == '#') {
                    continue;
                }
                tokenize(line, tokens);
                if (tokens.size() < 4) {
                    continue;
                }
                m_indices[(size_t)index * 3 + 0] = std::atoi(tokens[1].c_str());
                m_indices[(size_t)index * 3 + 1] = std::atoi(tokens[2].c_str());
                m_indices[(size_t)index * 3 + 2] = std::atoi(tokens[3].c_str());
                ++index;
            }
            if (index != numOfTriangles) {
                std::fclose(fp);
                return false;
            }
        }
    }

    std::fclose(fp);

    if (m_positions.empty() || m_indices.empty()) {
        return false;
    }

    m_numVertices = numOfVertices;
    m_numTriangles = numOfTriangles;

    m_aabbMin = m_positions[0];
    m_aabbMax = m_positions[0];
    for (const godot::Vector3& p : m_positions) {
        m_aabbMin.x = std::min(m_aabbMin.x, p.x);
        m_aabbMin.y = std::min(m_aabbMin.y, p.y);
        m_aabbMin.z = std::min(m_aabbMin.z, p.z);
        m_aabbMax.x = std::max(m_aabbMax.x, p.x);
        m_aabbMax.y = std::max(m_aabbMax.y, p.y);
        m_aabbMax.z = std::max(m_aabbMax.z, p.z);
    }

    return true;
}

bool CollisionMesh::EnsureGPUResourcesCreated() {
    if (m_gpuResourcesCreated) {
        return true;
    }
    if (!m_valid || m_numVertices <= 0 || m_numTriangles <= 0) {
        return false;
    }

    EI_Device* device = GetDevice();
    if (!device) {
        godot::UtilityFunctions::push_warning("CollisionMesh: EnsureGPUResourcesCreated: EI_Device is null");
        return false;
    }

    godot::RenderingDevice* rd = device->GetLocalRenderingDevice();
    if (!rd) {
        godot::UtilityFunctions::push_warning("CollisionMesh: EnsureGPUResourcesCreated: RenderingDevice is null");
        return false;
    }

    EI_CommandContext& ctx = device->GetCurrentCommandContext();
    ctx.set_rd(rd);

    // Interleaved position(float4)+normal(float4) per vertex, matching AMD's
    // TressFXBoneSkinning::GetSizeOfMeshElement() convention (32 bytes/vertex:
    // xyz+pad, xyz+pad) so a later skinning kernel can consume this unchanged.
    constexpr size_t kVertexBlockSize = 4 * sizeof(float) + 4 * sizeof(float);
    std::vector<uint8_t> vertexBlock((size_t)m_numVertices * kVertexBlockSize, 0);
    for (int i = 0; i < m_numVertices; ++i) {
        uint8_t* dst = &vertexBlock[(size_t)i * kVertexBlockSize];
        const float pos[3] = { m_positions[i].x, m_positions[i].y, m_positions[i].z };
        const float nrm[3] = { m_normals[i].x, m_normals[i].y, m_normals[i].z };
        std::memcpy(dst, pos, sizeof(pos));
        std::memcpy(dst + 4 * sizeof(float), nrm, sizeof(nrm));
    }

    m_restPositionsNormalsSRV = device->CreateBufferResource((int)kVertexBlockSize, m_numVertices, 0, "CollisionMeshInitialPositions");
    ctx.UpdateBuffer(m_restPositionsNormalsSRV.get(), vertexBlock.data());

    m_boneSkinningDataSRV = device->CreateBufferResource((int)sizeof(VertexBoneData), m_numVertices, 0, "CollisionMeshBoneSkinningData");
    ctx.UpdateBuffer(m_boneSkinningDataSRV.get(), m_boneData.data());

    // UAV-capable skinned-output buffer; seeded with the rest pose (no
    // skinning kernel writes it yet -- deliberately inert this subtask).
    m_skinnedPositionsUAV = device->CreateBufferResource((int)kVertexBlockSize, m_numVertices, EI_BF_NEEDSUAV, "CollisionMeshSkinnedPositions");
    ctx.UpdateBuffer(m_skinnedPositionsUAV.get(), vertexBlock.data());

    const int numIndices = m_numTriangles * 3;
    m_triangleIndicesSRV = device->CreateBufferResource((int)sizeof(int32_t), numIndices, EI_BF_INDEXBUFFER, "CollisionMeshTriangleIndices");
    ctx.UpdateBuffer(m_triangleIndicesSRV.get(), m_indices.data());

    // Safe no-op on the main RD (EI_CommandContext::EndAndSubmit() guards
    // is_main_rd); mirrors HairStrands::EnsureTressFXObjectCreated.
    device->EndAndSubmitCommandBuffer();
    device->FlushGPU();

    m_gpuResourcesCreated = true;
    return true;
}
