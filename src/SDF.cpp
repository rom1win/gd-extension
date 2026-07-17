#include "SDF.h"

#include "EngineInterface.h" // EI_Device, EI_Resource, EI_CommandContext, EI_BF_*, EI_Scene, GetDevice()
#include "TressFXLayouts.h"  // GetBoneSkinningMeshLayout() (still-compiled vendored layout table)

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
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

// Matches ConstBufferCS_BoneMatrix in
// demo/shaders/glsl/TressFXBoneSkinning.BoneSkinning.comp.glsl byte-for-byte
// (std140: ivec4 then a tightly-packed mat4 array, no padding needed since
// both are already 16-byte aligned).
constexpr int kMaxSkinningBones = 128; // see the kernel file's AMD_TRESSFX_MAX_NUM_BONES comment
struct BoneMatrixUBOData {
    int32_t numMeshVertices[4] = { 0, 0, 0, 0 };
    float boneMatrix[kMaxSkinningBones][16] = {};
};

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

    // UAV-capable skinned-output buffer; seeded with the rest pose. Written
    // per-frame by UpdateSkinning() once EnsureSkinningPSOCreated() has run.
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

godot::PackedByteArray CollisionMesh::SnapshotBoneMatrices() const {
    // MAIN thread only (reads the Skeleton3D through EI_Scene). Mirrors
    // HairStrands::SnapshotBoneMatrices exactly: both consume the same
    // per-skeleton-bone-index matrix array, so the same convention holds
    // (per-vertex boneIndex values already ARE skeleton bone indices --
    // LoadTfxMesh() resolved them at parse time via GetBoneIdByName).
    godot::PackedByteArray out;
    if (!m_pScene) {
        return out;
    }
    const std::vector<XMMATRIX>& mats = m_pScene->GetWorldSpaceSkeletonMats(m_skinNumber);
    if (mats.empty()) {
        return out;
    }
    out.resize((int64_t)mats.size() * (int64_t)sizeof(XMMATRIX));
    std::memcpy(out.ptrw(), mats.data(), (size_t)out.size());
    return out;
}

bool CollisionMesh::EnsureSkinningPSOCreated() {
    if (m_skinningPSOCreated) {
        return true;
    }
    if (!m_valid || !m_gpuResourcesCreated) {
        return false;
    }

    EI_Device* device = GetDevice();
    if (!device) {
        return false;
    }
    godot::RenderingDevice* rd = device->GetLocalRenderingDevice();
    if (!rd) {
        return false;
    }

    EI_BindLayout* boneSkinningLayout = GetBoneSkinningMeshLayout();
    if (!boneSkinningLayout) {
        // Global TressFX layouts not initialized yet (Simulation::Initialize()
        // creates them); try again next call.
        return false;
    }

    m_boneMatrixUBO = device->CreateBufferResource((int)sizeof(BoneMatrixUBOData), 1, EI_BF_UNIFORMBUFFER, "CollisionMeshBoneMatrixUBO");

    // Order MUST match CreateBoneSkinningLayout()'s resource list
    // (TressFXLayouts.cpp): boneSkinningData, initialVertexPositions,
    // collMeshVertexPositions (UAV), then the uniform buffer.
    EI_BindSetDescription bindSetDesc;
    bindSetDesc.resources = {
        m_boneSkinningDataSRV.get(),
        m_restPositionsNormalsSRV.get(),
        m_skinnedPositionsUAV.get(),
        m_boneMatrixUBO.get(),
    };
    m_boneSkinningBindSet = device->CreateBindSet(boneSkinningLayout, bindSetDesc);

    EI_BindLayout* layouts[] = { boneSkinningLayout };
    m_boneSkinningPSO = device->CreateComputeShaderPSO("TressFXBoneSkinning", "BoneSkinning", layouts, 1);

    m_skinningPSOCreated = (m_boneSkinningPSO && m_boneSkinningPSO->pipeline.is_valid());
    return m_skinningPSOCreated;
}

void CollisionMesh::UpdateSkinning(EI_CommandContext& commandContext, const godot::PackedByteArray& bone_matrices_snapshot) {
    if (!EnsureSkinningPSOCreated()) {
        return;
    }
    if (bone_matrices_snapshot.size() < (int64_t)sizeof(XMMATRIX)) {
        return;
    }

    BoneMatrixUBOData ubo;
    ubo.numMeshVertices[0] = m_numVertices;
    const int available = (int)(bone_matrices_snapshot.size() / (int64_t)sizeof(XMMATRIX));
    const int count = std::min(available, kMaxSkinningBones);
    std::memcpy(ubo.boneMatrix, bone_matrices_snapshot.ptr(), (size_t)count * sizeof(float) * 16);

    commandContext.UpdateBuffer(m_boneMatrixUBO.get(), &ubo);

    commandContext.BindPSO(m_boneSkinningPSO.get());
    EI_BindSet* bindSets[] = { m_boneSkinningBindSet.get() };
    commandContext.BindSets(m_boneSkinningPSO.get(), 1, bindSets);

    const int numGroups = (int)std::ceil((float)m_numVertices / 64.0f);
    commandContext.Dispatch(numGroups);

    // Mirrors TressFXBoneSkinning.cpp's Update(): a UAV->UAV barrier so any
    // pass reading the skinned buffer this frame observes the write.
    EI_Barrier flushSkinnedVerts[] = { { m_skinnedPositionsUAV.get(), EI_STATE_UAV, EI_STATE_UAV } };
    commandContext.SubmitBarrier(1, flushSkinnedVerts);
}

godot::RID CollisionMesh::GetSkinnedPositionsRID() const {
    return m_skinnedPositionsUAV ? m_skinnedPositionsUAV->rid : godot::RID();
}

godot::Vector3 CollisionMesh::GetRestPosition(int i) const {
    if (i < 0 || i >= (int)m_positions.size()) {
        return godot::Vector3();
    }
    return m_positions[i];
}

godot::Vector3 CollisionMesh::CpuSkinVertex(int vertexIndex, const godot::PackedByteArray& bone_matrices_snapshot) const {
    if (vertexIndex < 0 || vertexIndex >= (int)m_positions.size() || vertexIndex >= (int)m_boneData.size()) {
        return godot::Vector3();
    }
    if (bone_matrices_snapshot.size() < (int64_t)sizeof(XMMATRIX)) {
        return godot::Vector3();
    }

    const XMMATRIX* matrices = reinterpret_cast<const XMMATRIX*>(bone_matrices_snapshot.ptr());
    const int available = (int)(bone_matrices_snapshot.size() / (int64_t)sizeof(XMMATRIX));

    // Same clamp/zero-pad rules as the GLSL kernel: indices are clamped into
    // [0, kMaxSkinningBones-1]; anything past what UpdateSkinning() actually
    // wrote into the UBO reads as a zero matrix (the UBO's unwritten tail).
    auto boneMatrix = [&](int idx) -> XMMATRIX {
        const int clamped = std::clamp(idx, 0, kMaxSkinningBones - 1);
        if (clamped < available) {
            return matrices[clamped];
        }
        return XMMATRIX();
    };

    const VertexBoneData& bd = m_boneData[vertexIndex];
    const float w0 = bd.weight[0];
    XMMATRIX bone_matrix = boneMatrix((int)bd.boneIndex[0]) * w0;
    float weight_sum = w0;
    for (int k = 1; k < 4; ++k) {
        if (bd.weight[k] > 0.0f) {
            bone_matrix += boneMatrix((int)bd.boneIndex[k]) * bd.weight[k];
            weight_sum += bd.weight[k];
        }
    }
    if (weight_sum > 1e-6f) {
        bone_matrix /= weight_sum;
    }

    const godot::Vector3& p = m_positions[vertexIndex];
    XMVECTOR posV{ p.x, p.y, p.z, 1.0f };
    // Same packing convention as src/GodotScene.cpp: XMVector4Transform(v, M)
    // on this row-major-packed M reproduces the GLSL kernel's (mat4(M) * vec4(v,1)).
    const XMVECTOR skinned = XMVector4Transform(posV, bone_matrix);
    return godot::Vector3(skinned.x, skinned.y, skinned.z);
}
