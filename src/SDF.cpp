#include "SDF.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <algorithm>

static godot::String bytes_to_hex_prefix_sdf(const godot::PackedByteArray& bytes, int64_t max_bytes) {
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

static void log_file_probe_sdf(const std::string& path_utf8) {
    using namespace godot;

    const String path = String(path_utf8.c_str());
    String global = path;
    if (ProjectSettings::get_singleton()) {
        global = ProjectSettings::get_singleton()->globalize_path(path);
    }

    Ref<FileAccess> f;
    if (FileAccess::file_exists(path)) {
        f = FileAccess::open(path, FileAccess::READ);
    }
    if (!f.is_valid() && FileAccess::file_exists(global)) {
        f = FileAccess::open(global, FileAccess::READ);
    }

    if (!f.is_valid()) {
        UtilityFunctions::print(
            String("CollisionMesh: tfxmesh open FAILED path='") + path +
            String("' global='") + global +
            String("' open_error=") + String::num_int64((int)FileAccess::get_open_error()));
        return;
    }

    const uint64_t len = f->get_length();
    const int64_t probe_len = (int64_t)std::min<uint64_t>(len, 16);
    f->seek(0);
    const PackedByteArray head = f->get_buffer(probe_len);

    UtilityFunctions::print(
        String("CollisionMesh: tfxmesh opened ok path='") + path +
        String("' global='") + global +
        String("' bytes=") + String::num_int64((int64_t)len) +
        String(" head[") + String::num_int64(probe_len) + String("]=") + bytes_to_hex_prefix_sdf(head, 16));
}

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
    // First incremental implementation: no GPU resources yet.

        godot::UtilityFunctions::print(
                godot::String("CollisionMesh: constructed name='") + godot::String(m_name.c_str()) +
                godot::String("' tfxmesh='") + godot::String(m_tfxmeshFilePath.c_str()) +
                godot::String("' followBone='") + godot::String(m_followBone.c_str()) +
                godot::String("' cells=") + godot::String::num_int64(m_numCellsInXAxis) +
                godot::String(" margin=") + godot::String::num(m_SDFCollMargin, 3));

            // Step 0: prove we can read the collision mesh asset the user configured.
            log_file_probe_sdf(m_tfxmeshFilePath);
}

void CollisionMesh::SkinTheMesh(EI_CommandContext& context, double fTime) {
    (void)context;
    (void)fTime;
}

void CollisionMesh::AccumulateSDF(EI_CommandContext& context, TressFXSDFCollisionSystem& sdfCollisionSystem) {
    (void)context;
    (void)sdfCollisionSystem;
}

void CollisionMesh::ApplySDF(EI_CommandContext& context, TressFXSDFCollisionSystem& sdfCollisionSystem, TressFXHairObject* strands) {
    (void)context;
    (void)sdfCollisionSystem;
    (void)strands;
}

void CollisionMesh::GenerateIsoSurface(EI_CommandContext& context) {
    (void)context;
}

void CollisionMesh::DrawIsoSurface(EI_CommandContext& context) {
    (void)context;
}

void CollisionMesh::DrawMesh(EI_CommandContext& context) {
    (void)context;
}
