#include "SDF.h"

#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <algorithm>
#include <fstream>

#include "TressFX/TressFXBoneSkinning.h"

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

CollisionMesh::~CollisionMesh() = default;

static std::string resolve_os_path_for_collision_mesh(const std::string& path_utf8, const std::string& name_utf8) {
    using namespace godot;

    const String path = String(path_utf8.c_str());
    String global = path;
    if (ProjectSettings::get_singleton()) {
        global = ProjectSettings::get_singleton()->globalize_path(path);
    }

    // 1) Try to use a real filesystem path directly.
    {
        const CharString global_utf8 = global.utf8();
        const char* global_cstr = global_utf8.get_data();
        if (global_cstr && global_cstr[0] != '\0') {
            std::ifstream test(global_cstr);
            if (test.is_open()) {
                return std::string(global_cstr);
            }
        }
    }

    // 2) Fallback: copy from Godot VFS into user:// (which is a real filesystem path), then return its globalized path.
    Ref<FileAccess> in;
    if (FileAccess::file_exists(path)) {
        in = FileAccess::open(path, FileAccess::READ);
    }
    if (!in.is_valid() && FileAccess::file_exists(global)) {
        in = FileAccess::open(global, FileAccess::READ);
    }
    if (!in.is_valid()) {
        return {};
    }

    const uint64_t len = in->get_length();
    in->seek(0);
    const PackedByteArray bytes = in->get_buffer((int64_t)len);

    // Ensure cache directory exists.
    String cache_dir = "user://tressfx_cache";
    if (ProjectSettings::get_singleton()) {
        const String cache_dir_abs = ProjectSettings::get_singleton()->globalize_path(cache_dir);
        DirAccess::make_dir_recursive_absolute(cache_dir_abs);
    }

    const uint64_t suffix = (uint64_t)std::hash<std::string>{}(path_utf8);
    const String cache_path = cache_dir + String("/") + String(name_utf8.c_str()) + String("_") + String::num_int64((int64_t)suffix) + String(".tfxmesh");

    Ref<FileAccess> out = FileAccess::open(cache_path, FileAccess::WRITE);
    if (!out.is_valid()) {
        return {};
    }
    out->store_buffer(bytes);
    out->flush();
    out->close();

    String cache_abs = cache_path;
    if (ProjectSettings::get_singleton()) {
        cache_abs = ProjectSettings::get_singleton()->globalize_path(cache_path);
    }
    const CharString cache_abs_utf8 = cache_abs.utf8();
    const char* cache_abs_cstr = cache_abs_utf8.get_data();
    if (!cache_abs_cstr || cache_abs_cstr[0] == '\0') {
        return {};
    }

    return std::string(cache_abs_cstr);
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

            // Step 1: parse/load the collision mesh through the TressFX bone-skinning path (CPU-side only).
            if (!m_pScene) {
                godot::UtilityFunctions::print("CollisionMesh: no EI_Scene; skipping TressFXBoneSkinning load");
                return;
            }

            const std::string os_path = resolve_os_path_for_collision_mesh(m_tfxmeshFilePath, m_name.empty() ? std::string("collision") : m_name);
            if (os_path.empty()) {
                godot::UtilityFunctions::print(
                    godot::String("CollisionMesh: failed to resolve OS path for '") + godot::String(m_tfxmeshFilePath.c_str()) + godot::String("'"));
                return;
            }

            m_boneSkinning = std::make_unique<TressFXBoneSkinning>();
            const bool ok = m_boneSkinning->LoadTressFXCollisionMeshData(
                m_pScene,
                os_path.c_str(),
                m_skinNumber,
                m_followBone.c_str());

            if (!ok) {
                godot::UtilityFunctions::print(
                    godot::String("CollisionMesh: TressFXBoneSkinning::LoadTressFXCollisionMeshData FAILED for '") +
                    godot::String(m_tfxmeshFilePath.c_str()) +
                    godot::String("' (os_path='") + godot::String(os_path.c_str()) + godot::String("')"));
                m_boneSkinning.reset();
                return;
            }

            godot::UtilityFunctions::print(
                godot::String("CollisionMesh: loaded ok verts=") + godot::String::num_int64(m_boneSkinning->GetNumMeshVertices()) +
                godot::String(" tris=") + godot::String::num_int64(m_boneSkinning->GetNumMeshTriangle()) +
                godot::String(" followBone='") + godot::String(m_followBone.c_str()) +
                godot::String("'") +
                godot::String(" os_path='") + godot::String(os_path.c_str()) + godot::String("'"));
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
