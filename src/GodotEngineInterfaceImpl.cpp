#include "EngineInterface.h"

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_attachment_format.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

using namespace godot;

static EI_Device* g_device_singleton = nullptr;

EI_Device* GetDevice() {
    return g_device_singleton;
}

void InitializeGodotEngineInterface() {
    if (g_device_singleton) {
        return;
    }
    g_device_singleton = memnew(EI_Device);
}

void ShutdownGodotEngineInterface() {
    if (!g_device_singleton) {
        return;
    }
    memdelete(g_device_singleton);
    g_device_singleton = nullptr;
}

EI_BindSet::~EI_BindSet() {
    if (rd && rid.is_valid()) {
        rd->free_rid(rid);
    }
}

EI_PSO::~EI_PSO() {
    if (rd) {
        if (pipeline.is_valid()) {
            rd->free_rid(pipeline);
        }
        if (shader.is_valid()) {
            rd->free_rid(shader);
        }
    }
}

EI_Resource::~EI_Resource() {
    if (rd && rid.is_valid()) {
        rd->free_rid(rid);
    }
}

EI_Device::EI_Device() {
    // IMPORTANT: do not touch Godot singletons here.
    // EI_Device is a global static and its constructor runs during DLL load.
    // Accessing RenderingServer/UtilityFunctions at that time can fail and cause ERROR_DLL_INIT_FAILED.
}

EI_Device::~EI_Device() = default;

RenderingDevice* EI_Device::get_rd() {
    if (m_local_rd) {
        return m_local_rd;
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        return nullptr;
    }

    // NOTE: get_rendering_device() returns the main render device which is not safe to
    // submit/sync from arbitrary threads. For compute-first bring-up and CPU readback,
    // we use a local RenderingDevice.
    m_local_rd = rs->create_local_rendering_device();
    return m_local_rd;
}

void EI_CommandContext::BeginComputeIfNeeded() {
    if (!rd) {
        return;
    }
    if (compute_list != -1) {
        return;
    }
    compute_list = rd->compute_list_begin();
}

void EI_CommandContext::EndAndSubmit() {
    if (!rd) {
        return;
    }
    if (compute_list == -1) {
        return;
    }

    rd->compute_list_end();
    compute_list = -1;
    bound_pso = nullptr;

    rd->submit();
}

void EI_CommandContext::SubmitBarrier(int /*numBarriers*/, EI_Barrier* /*barriers*/) {
    // For compute-first bring-up, treat barriers as a generic compute barrier.
    if (!rd) {
        return;
    }
    BeginComputeIfNeeded();
    rd->compute_list_add_barrier(compute_list);
}

void EI_CommandContext::BindPSO(EI_PSO* pso) {
    if (!rd || !pso || !pso->pipeline.is_valid()) {
        return;
    }
    BeginComputeIfNeeded();
    rd->compute_list_bind_compute_pipeline(compute_list, pso->pipeline);
    bound_pso = pso;
}

void EI_CommandContext::BindSets(EI_PSO* /*pso*/, int numBindSets, EI_BindSet** bindSets) {
    if (!rd || numBindSets <= 0 || !bindSets) {
        return;
    }
    BeginComputeIfNeeded();

    // Lazily create uniform sets once a PSO is bound, since Godot requires the shader RID.
    // This keeps the TressFX API shape intact (CreateBindSet happens before Create*PSO in many places).
    if (bound_pso && bound_pso->shader.is_valid()) {
        for (int i = 0; i < numBindSets; ++i) {
            EI_BindSet* s = bindSets[i];
            if (!s) {
                continue;
            }
            if (!s->rid.is_valid() && s->uniforms.size() > 0) {
                s->rid = rd->uniform_set_create(s->uniforms, bound_pso->shader, s->set_index);
            }
        }
    }

    for (int i = 0; i < numBindSets; ++i) {
        EI_BindSet* s = bindSets[i];
        if (!s || !s->rid.is_valid()) {
            continue;
        }
        rd->compute_list_bind_uniform_set(compute_list, s->rid, s->set_index);
    }
}

void EI_CommandContext::Dispatch(int numGroups) {
    if (!rd || numGroups <= 0) {
        return;
    }
    BeginComputeIfNeeded();
    rd->compute_list_dispatch(compute_list, (uint32_t)numGroups, 1, 1);
}

void EI_CommandContext::UpdateBuffer(EI_Resource* res, void* data) {
    if (!rd || !res || !res->rid.is_valid() || res->size_bytes == 0 || !data) {
        return;
    }

    PackedByteArray bytes;
    bytes.resize((int)res->size_bytes);
    std::memcpy(bytes.ptrw(), data, res->size_bytes);
    rd->buffer_update(res->rid, 0, res->size_bytes, bytes);
}

void EI_CommandContext::ClearUint32Image(EI_Resource* res, uint32_t value) {
    // Godot's texture_clear() uses Color and isn't suitable for integer textures.
    // For bring-up, we do a CPU-side update (slow but correct).
    if (!rd || !res || !res->rid.is_valid() || res->GetWidth() <= 0 || res->GetHeight() <= 0) {
        return;
    }

    const int w = res->GetWidth();
    const int h = res->GetHeight();
    const size_t bytes_per_pixel = 4;
    const size_t size_bytes = (size_t)w * (size_t)h * bytes_per_pixel;

    PackedByteArray bytes;
    bytes.resize((int)size_bytes);
    uint8_t* dst = bytes.ptrw();
    for (size_t i = 0; i < size_bytes; i += 4) {
        dst[i + 0] = (uint8_t)(value & 0xFF);
        dst[i + 1] = (uint8_t)((value >> 8) & 0xFF);
        dst[i + 2] = (uint8_t)((value >> 16) & 0xFF);
        dst[i + 3] = (uint8_t)((value >> 24) & 0xFF);
    }

    // NOTE: We assume layer 0 for now (the current Godot EI_Resource does not track array layers).
    rd->texture_update(res->rid, 0, bytes);
}

void EI_CommandContext::ClearFloat32Image(EI_Resource* res, float value) {
    if (!rd || !res || !res->rid.is_valid()) {
        return;
    }
    // Best-effort: use texture_clear. Works for float/normalized formats.
    rd->texture_clear(res->rid, Color(value, value, value, value), 0, 1, 0, 1);
}

void EI_CommandContext::DrawIndexedInstanced(EI_PSO& /*pso*/, EI_IndexedDrawParams& /*drawParams*/) {
    // Not needed for compute-first milestone.
}

void EI_CommandContext::DrawInstanced(EI_PSO& /*pso*/, EI_DrawParams& /*drawParams*/) {
    // Not needed for compute-first milestone.
}

void EI_CommandContext::PushConstants(EI_PSO* /*pso*/, int size, void* data) {
    if (!rd || size <= 0 || !data) {
        return;
    }
    BeginComputeIfNeeded();
    PackedByteArray bytes;
    bytes.resize(size);
    std::memcpy(bytes.ptrw(), data, (size_t)size);
    rd->compute_list_set_push_constant(compute_list, bytes, (uint32_t)size);
}

std::unique_ptr<EI_Resource> EI_Device::CreateBufferResource(const int structSize, const int structCount, const unsigned int flags, const char* name) {
    RenderingDevice* rd = get_rd();
    if (!rd || structSize <= 0 || structCount <= 0) {
        UtilityFunctions::push_warning("EI_Device::CreateBufferResource: invalid args or no RenderingDevice");
        return std::make_unique<EI_Resource>();
    }

    const uint32_t size_bytes = (uint32_t)(structSize * structCount);
    RID buffer;

    if (flags & EI_BF_UNIFORMBUFFER) {
        buffer = rd->uniform_buffer_create(size_bytes);
    } else if (flags & EI_BF_VERTEXBUFFER) {
        // Vertex buffers are not required yet; store as storage buffer to unblock compute.
        buffer = rd->storage_buffer_create(size_bytes);
    } else if (flags & EI_BF_INDEXBUFFER) {
        // Index buffers are not required yet; store as storage buffer to unblock compute.
        buffer = rd->storage_buffer_create(size_bytes);
    } else {
        buffer = rd->storage_buffer_create(size_bytes);
    }

    if (buffer.is_valid() && name && name[0] != '\0') {
        rd->set_resource_name(buffer, String(name));
    }

    auto res = std::make_unique<EI_Resource>();
    res->rd = rd;
    res->rid = buffer;
    res->size_bytes = size_bytes;
    res->m_ResourceType = EI_ResourceType::Buffer;
    return res;
}

std::unique_ptr<EI_Resource> EI_Device::CreateUint32Resource(const int width, const int height, const size_t arraySize, const char* name, uint32_t ClearValue) {
    RenderingDevice* rd = get_rd();
    if (!rd || width <= 0 || height <= 0) {
        UtilityFunctions::push_warning("EI_Device::CreateUint32Resource: invalid args or no RenderingDevice");
        return std::make_unique<EI_Resource>();
    }

    Ref<RDTextureFormat> fmt;
    fmt.instantiate();
    fmt->set_width((uint32_t)width);
    fmt->set_height((uint32_t)height);
    fmt->set_depth(1);

    const uint32_t layers = (uint32_t)std::max<size_t>(1, arraySize);
    if (layers > 1) {
        fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
        fmt->set_array_layers(layers);
    } else {
        fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
        fmt->set_array_layers(1);
    }

    fmt->set_mipmaps(1);
    fmt->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
    fmt->set_format(RenderingDevice::DATA_FORMAT_R32_UINT);
    fmt->set_usage_bits(
        RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
        RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT
    );

    Ref<RDTextureView> view;
    view.instantiate();

    RID tex = rd->texture_create(fmt, view);
    if (tex.is_valid() && name && name[0] != '\0') {
        rd->set_resource_name(tex, String(name));
    }

    auto res = std::make_unique<EI_Resource>();
    res->rd = rd;
    res->rid = tex;
    res->width = width;
    res->height = height;
    res->m_ResourceType = EI_ResourceType::Texture;

    // Best-effort clear for bring-up. For array textures, clear each layer.
    if (tex.is_valid()) {
        EI_CommandContext& ctx = GetCurrentCommandContext();
        ctx.set_rd(rd);
        for (uint32_t layer = 0; layer < layers; ++layer) {
            const size_t bytes_per_pixel = 4;
            const size_t size_bytes = (size_t)width * (size_t)height * bytes_per_pixel;
            PackedByteArray bytes;
            bytes.resize((int)size_bytes);
            uint8_t* dst = bytes.ptrw();
            for (size_t i = 0; i < size_bytes; i += 4) {
                dst[i + 0] = (uint8_t)(ClearValue & 0xFF);
                dst[i + 1] = (uint8_t)((ClearValue >> 8) & 0xFF);
                dst[i + 2] = (uint8_t)((ClearValue >> 16) & 0xFF);
                dst[i + 3] = (uint8_t)((ClearValue >> 24) & 0xFF);
            }
            rd->texture_update(tex, layer, bytes);
        }
    }

    return res;
}

std::unique_ptr<EI_Resource> EI_Device::CreateRenderTargetResource(const int /*width*/, const int /*height*/, const size_t /*channels*/, const size_t /*channelSize*/, const char* /*name*/, AMD::float4* /*ClearValues*/) {
    // TODO: implement when rendering is integrated.
    return std::make_unique<EI_Resource>();
}

std::unique_ptr<EI_Resource> EI_Device::CreateDepthResource(const int /*width*/, const int /*height*/, const char* /*name*/) {
    // TODO: implement when rendering is integrated.
    return std::make_unique<EI_Resource>();
}

std::unique_ptr<EI_Resource> EI_Device::CreateResourceFromFile(const char* /*szFilename*/, bool /*useSRGB*/) {
    // TODO: implement when textures are needed.
    return std::make_unique<EI_Resource>();
}

std::unique_ptr<EI_Resource> EI_Device::CreateSampler(EI_Filter MinFilter, EI_Filter MaxFilter, EI_Filter /*MipFilter*/, EI_AddressMode AddressMode) {
    RenderingDevice* rd = get_rd();
    if (!rd) {
        UtilityFunctions::push_warning("EI_Device::CreateSampler: no RenderingDevice");
        return std::make_unique<EI_Resource>();
    }

    Ref<RDSamplerState> st;
    st.instantiate();

    // Filter mapping (minimal subset).
    const RenderingDevice::SamplerFilter minf = (MinFilter == EI_Filter::Point) ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR;
    const RenderingDevice::SamplerFilter magf = (MaxFilter == EI_Filter::Point) ? RenderingDevice::SAMPLER_FILTER_NEAREST : RenderingDevice::SAMPLER_FILTER_LINEAR;
    st->set_min_filter(minf);
    st->set_mag_filter(magf);

    // Address mode mapping.
    const RenderingDevice::SamplerRepeatMode repeat = (AddressMode == EI_AddressMode::ClampEdge)
        ? RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
        : RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT;
    st->set_repeat_u(repeat);
    st->set_repeat_v(repeat);
    st->set_repeat_w(repeat);

    RID samp = rd->sampler_create(st);

    auto res = std::make_unique<EI_Resource>();
    res->rd = rd;
    res->rid = samp;
    res->m_ResourceType = EI_ResourceType::Sampler;
    return res;
}

std::unique_ptr<EI_BindLayout> EI_Device::CreateLayout(const EI_LayoutDescription& description) {
    auto layout = std::make_unique<EI_BindLayout>();
    layout->description = description;
    layout->set_index = -1;
    return layout;
}

static RenderingDevice::UniformType map_uniform_type(EI_ResourceTypeEnum t) {
    switch (t) {
        case EI_RESOURCETYPE_BUFFER_RW:
        case EI_RESOURCETYPE_BUFFER_RO:
            return RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER;
        case EI_RESOURCETYPE_UNIFORM:
            return RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER;
        case EI_RESOURCETYPE_IMAGE_RW:
        case EI_RESOURCETYPE_IMAGE_RO:
            return RenderingDevice::UNIFORM_TYPE_IMAGE;
        case EI_RESOURCETYPE_SAMPLER:
            return RenderingDevice::UNIFORM_TYPE_SAMPLER;
        default:
            return RenderingDevice::UNIFORM_TYPE_MAX;
    }
}

std::unique_ptr<EI_BindSet> EI_Device::CreateBindSet(EI_BindLayout* layout, EI_BindSetDescription& bindSet) {
    RenderingDevice* rd = get_rd();
    if (!rd || !layout) {
        UtilityFunctions::push_warning("EI_Device::CreateBindSet: invalid args or no RenderingDevice");
        return std::make_unique<EI_BindSet>();
    }

    if (layout->set_index < 0) {
        // If the PSO has not assigned indices yet, default to 0.
        UtilityFunctions::push_warning("EI_Device::CreateBindSet: layout set_index not assigned; defaulting to 0");
        layout->set_index = 0;
    }

    TypedArray<RDUniform> uniforms;
    const int n = (int)layout->description.resources.size();
    uniforms.resize(n);

    for (int i = 0; i < n; ++i) {
        const EI_ResourceDescription& desc = layout->description.resources[i];
        EI_Resource* res = (i < (int)bindSet.resources.size()) ? bindSet.resources[i] : nullptr;

        Ref<RDUniform> u;
        u.instantiate();
        u->set_binding(desc.binding);

        const RenderingDevice::UniformType ut = map_uniform_type(desc.type);
        if (ut == RenderingDevice::UNIFORM_TYPE_MAX) {
            UtilityFunctions::push_warning(String("EI_Device::CreateBindSet: unsupported uniform type for '") + desc.name + String("'"));
            u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
        } else {
            u->set_uniform_type(ut);
        }

        if (res && res->rid.is_valid()) {
            u->add_id(res->rid);
        } else {
            UtilityFunctions::push_warning(String("EI_Device::CreateBindSet: missing resource for '") + desc.name + String("'"));
        }

        uniforms[i] = u;
    }

    auto set = std::make_unique<EI_BindSet>();
    set->rd = rd;
    set->set_index = (uint32_t)layout->set_index;
    set->uniforms = uniforms;
    return set;
}

EI_Resource* EI_Device::GetDefaultWhiteTexture() {
    RenderingDevice* rd = get_rd();
    if (!rd) {
        return nullptr;
    }

    if (m_default_white_texture && m_default_white_texture->rid.is_valid()) {
        return m_default_white_texture.get();
    }

    Ref<RDTextureFormat> fmt;
    fmt.instantiate();
    fmt->set_width(1);
    fmt->set_height(1);
    fmt->set_depth(1);
    fmt->set_array_layers(1);
    fmt->set_mipmaps(1);
    fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
    fmt->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
    fmt->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    fmt->set_usage_bits(
        RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT
    );

    Ref<RDTextureView> view;
    view.instantiate();

    RID tex = rd->texture_create(fmt, view);
    if (tex.is_valid()) {
        rd->set_resource_name(tex, "tressfx_default_white");

        PackedByteArray px;
        px.resize(4);
        uint8_t* p = px.ptrw();
        p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
        rd->texture_update(tex, 0, px);
    }

    m_default_white_texture = std::make_unique<EI_Resource>();
    m_default_white_texture->rd = rd;
    m_default_white_texture->rid = tex;
    m_default_white_texture->width = 1;
    m_default_white_texture->height = 1;
    m_default_white_texture->m_ResourceType = EI_ResourceType::Texture;
    return m_default_white_texture.get();
}

void EI_Device::BeginRenderPass(EI_CommandContext& /*commandContext*/, const EI_RenderTargetSet* /*pRenderPassSet*/, const wchar_t* /*pPassName*/, uint32_t /*width*/, uint32_t /*height*/) {
    // TODO: implement when integrating draw passes.
}

void EI_Device::EndRenderPass(EI_CommandContext& /*commandContext*/) {
    // TODO: implement when integrating draw passes.
}

void EI_Device::SetViewportAndScissor(EI_CommandContext& /*commandContext*/, uint32_t /*topX*/, uint32_t /*topY*/, uint32_t /*width*/, uint32_t /*height*/) {
    // TODO: implement when integrating draw passes.
}

static PackedByteArray load_file_bytes_or_empty(const String& path) {
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    if (!f.is_valid()) {
        return PackedByteArray();
    }
    const int64_t len = f->get_length();
    if (len <= 0) {
        return PackedByteArray();
    }
    return f->get_buffer(len);
}

static String load_file_text_or_empty(const String& path) {
    Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
    if (!f.is_valid()) {
        return String();
    }
    return f->get_as_text();
}

static String make_spirv_path(const char* shader_name, const char* stage_ext) {
    if (!shader_name || shader_name[0] == '\0') {
        return String();
    }

    String s(shader_name);
    s = s.strip_edges();

    // If the caller passed an explicit path, use it.
    if (s.begins_with("res://") || s.begins_with("user://")) {
        return s;
    }

    // If the caller provided an explicit .spv file name, use it under the pack folder.
    if (s.ends_with(".spv")) {
        return String("res://shaders/spirv/") + s;
    }

    return String("res://shaders/spirv/") + s + String(".") + String(stage_ext) + String(".spv");
}

static String strip_known_shader_extensions(String s) {
    s = s.strip_edges();
    if (s.ends_with(".spv")) {
        s = s.substr(0, s.length() - 4);
    }
    if (s.ends_with(".hlsl")) {
        s = s.substr(0, s.length() - 5);
    }
    if (s.ends_with(".glsl")) {
        s = s.substr(0, s.length() - 5);
    }
    if (s.ends_with(".comp") || s.ends_with(".vert") || s.ends_with(".frag")) {
        s = s.substr(0, s.length() - 5);
    }
    return s;
}

static String make_spirv_path_compute(const char* shader_name, const char* entry_point) {
    if (!shader_name || shader_name[0] == '\0') {
        return String();
    }

    String s(shader_name);
    s = s.strip_edges();

    // Explicit paths are respected.
    if (s.begins_with("res://") || s.begins_with("user://")) {
        return s;
    }

    // Allow callers to pass explicit .spv under the pack folder.
    if (s.ends_with(".spv")) {
        return String("res://shaders/spirv/") + s;
    }

    const String base = strip_known_shader_extensions(s);

    String ep;
    if (entry_point && entry_point[0] != '\0') {
        ep = String(entry_point).strip_edges();
    }

    if (!ep.is_empty()) {
        return String("res://shaders/spirv/") + base + String(".") + ep + String(".comp.spv");
    }

    return String("res://shaders/spirv/") + base + String(".comp.spv");
}

static String make_glsl_path_compute(const char* shader_name, const char* entry_point) {
    // Source fallback mirrors the SPIR-V naming.
    if (!shader_name || shader_name[0] == '\0') {
        return String();
    }

    String s(shader_name);
    s = s.strip_edges();

    if (s.begins_with("res://") || s.begins_with("user://")) {
        return s;
    }

    const String base = strip_known_shader_extensions(s);
    String ep;
    if (entry_point && entry_point[0] != '\0') {
        ep = String(entry_point).strip_edges();
    }

    if (!ep.is_empty()) {
        return String("res://shaders/glsl/") + base + String(".") + ep + String(".comp.glsl");
    }

    return String("res://shaders/glsl/") + base + String(".comp.glsl");
}

static void warn_missing_shader_once(const String& key, const String& message) {
    static std::unordered_set<std::string> s_once;
    const std::string k = std::string(key.utf8().get_data());
    if (s_once.find(k) != s_once.end()) {
        return;
    }
    s_once.insert(k);
    UtilityFunctions::push_warning(message);
}

// For RenderingDevice, we can compile SPIR-V at runtime from GLSL sources.
// We map TressFX's (shaderName, entryPoint) to a single GLSL file per kernel.
// Example:
//   shaderName="TressFXSimulation.hlsl", entryPoint="VelocityShockPropagation"
//   stage="comp" -> res://shaders/glsl/TressFXSimulation.VelocityShockPropagation.comp.glsl
static String make_glsl_path(const char* shader_name, const char* entry_point, const char* stage_ext) {
    if (!shader_name || shader_name[0] == '\0') {
        return String();
    }

    String base(shader_name);
    base = base.strip_edges();
    if (base.begins_with("res://") || base.begins_with("user://")) {
        return base;
    }

    // Strip common extensions.
    if (base.ends_with(".hlsl")) {
        base = base.substr(0, base.length() - 5);
    } else if (base.ends_with(".glsl")) {
        base = base.substr(0, base.length() - 5);
    }

    String ep;
    if (entry_point && entry_point[0] != '\0') {
        ep = String(entry_point);
        ep = ep.strip_edges();
    }

    // Prefer per-entrypoint files when an entrypoint was provided.
    if (!ep.is_empty()) {
        return String("res://shaders/glsl/") + base + String(".") + ep + String(".") + String(stage_ext) + String(".glsl");
    }

    return String("res://shaders/glsl/") + base + String(".") + String(stage_ext) + String(".glsl");
}

static Ref<RDShaderSPIRV> compile_spirv_from_glsl(RenderingDevice* rd, RenderingDevice::ShaderStage stage, const String& path, const String& debug_name) {
    if (!rd || path.is_empty()) {
        return Ref<RDShaderSPIRV>();
    }

    const String source_text = load_file_text_or_empty(path);
    if (source_text.is_empty()) {
        return Ref<RDShaderSPIRV>();
    }

    Ref<RDShaderSource> src;
    src.instantiate();
    src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    src->set_stage_source(stage, source_text);

    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src, true);
    if (!spirv.is_valid()) {
        UtilityFunctions::push_warning(String("shader_compile_spirv_from_source failed: ") + debug_name + String(" (") + path + String(")"));
        return Ref<RDShaderSPIRV>();
    }

    const String err = spirv->get_stage_compile_error(stage);
    if (!err.is_empty()) {
        UtilityFunctions::push_warning(String("GLSL compile error for ") + debug_name + String(" (") + path + String("): \n") + err);
        return Ref<RDShaderSPIRV>();
    }

    return spirv;
}

static RenderingDevice::RenderPrimitive map_primitive(EI_Topology topo) {
    switch (topo) {
        case EI_Topology::TriangleStrip:
            return RenderingDevice::RENDER_PRIMITIVE_TRIANGLE_STRIPS;
        case EI_Topology::TriangleList:
        default:
            return RenderingDevice::RENDER_PRIMITIVE_TRIANGLES;
    }
}

static RenderingDevice::BlendFactor map_blend_factor(EI_BlendFactor f) {
    switch (f) {
        case EI_BlendFactor::Zero: return RenderingDevice::BLEND_FACTOR_ZERO;
        case EI_BlendFactor::One: return RenderingDevice::BLEND_FACTOR_ONE;
        case EI_BlendFactor::SrcColor: return RenderingDevice::BLEND_FACTOR_SRC_COLOR;
        case EI_BlendFactor::InvSrcColor: return RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case EI_BlendFactor::DstColor: return RenderingDevice::BLEND_FACTOR_DST_COLOR;
        case EI_BlendFactor::InvDstColor: return RenderingDevice::BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case EI_BlendFactor::SrcAlpha: return RenderingDevice::BLEND_FACTOR_SRC_ALPHA;
        case EI_BlendFactor::InvSrcAlpha: return RenderingDevice::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case EI_BlendFactor::DstAlpha: return RenderingDevice::BLEND_FACTOR_DST_ALPHA;
        case EI_BlendFactor::InvDstAlpha: return RenderingDevice::BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default: return RenderingDevice::BLEND_FACTOR_ONE;
    }
}

static RenderingDevice::BlendOperation map_blend_op(EI_BlendOp op) {
    switch (op) {
        case EI_BlendOp::Subtract: return RenderingDevice::BLEND_OP_SUBTRACT;
        case EI_BlendOp::ReverseSubtract: return RenderingDevice::BLEND_OP_REVERSE_SUBTRACT;
        case EI_BlendOp::Min: return RenderingDevice::BLEND_OP_MINIMUM;
        case EI_BlendOp::Max: return RenderingDevice::BLEND_OP_MAXIMUM;
        case EI_BlendOp::Add:
        default:
            return RenderingDevice::BLEND_OP_ADD;
    }
}

std::unique_ptr<EI_PSO> EI_Device::CreateComputeShaderPSO(const char* shaderName, const char* entryPoint, EI_BindLayout** layouts, int numLayouts) {
    RenderingDevice* rd = get_rd();
    if (!rd) {
        UtilityFunctions::push_warning("EI_Device::CreateComputeShaderPSO: no RenderingDevice");
        return std::make_unique<EI_PSO>();
    }

    if (layouts) {
        for (int i = 0; i < numLayouts; ++i) {
            if (layouts[i] && layouts[i]->set_index < 0) {
                layouts[i]->set_index = i;
            }
        }
    }

    const String glsl_path = make_glsl_path_compute(shaderName, entryPoint);
    const String spv_path = make_spirv_path_compute(shaderName, entryPoint);
    if (glsl_path.is_empty() && spv_path.is_empty()) {
        UtilityFunctions::push_warning("EI_Device::CreateComputeShaderPSO: empty shader name");
        return std::make_unique<EI_PSO>();
    }

    const String debug_name = String(shaderName ? shaderName : "compute") + String("::") + String(entryPoint ? entryPoint : "main");

    // RenderingDevice ultimately consumes SPIR-V, so prefer loading bytecode if present.
    Ref<RDShaderSPIRV> spirv;
    {
        const PackedByteArray bytecode = load_file_bytes_or_empty(spv_path);
        if (!bytecode.is_empty()) {
            spirv.instantiate();
            spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_COMPUTE, bytecode);
        }
    }

    // Fallback: compile from GLSL source.
    if (!spirv.is_valid()) {
        spirv = compile_spirv_from_glsl(rd, RenderingDevice::SHADER_STAGE_COMPUTE, glsl_path, debug_name);
    }

    if (!spirv.is_valid()) {
        warn_missing_shader_once(
            debug_name,
            String("EI_Device::CreateComputeShaderPSO: missing shader for ") + debug_name +
                String(". SPIR-V=") + spv_path + String(" GLSL=") + glsl_path);
        return std::make_unique<EI_PSO>();
    }

    RID shader = rd->shader_create_from_spirv(spirv, String("tressfx_") + debug_name);
    if (!shader.is_valid()) {
        UtilityFunctions::push_warning(String("EI_Device::CreateComputeShaderPSO: shader_create_from_spirv failed: ") + debug_name);
        return std::make_unique<EI_PSO>();
    }

    RID pipeline = rd->compute_pipeline_create(shader);
    if (!pipeline.is_valid()) {
        UtilityFunctions::push_warning(String("EI_Device::CreateComputeShaderPSO: compute_pipeline_create failed: ") + debug_name);
        rd->free_rid(shader);
        return std::make_unique<EI_PSO>();
    }

    auto pso = std::make_unique<EI_PSO>();
    pso->rd = rd;
    pso->m_bp = EI_BP_COMPUTE;
    pso->shader = shader;
    pso->pipeline = pipeline;
    return pso;
}

std::unique_ptr<EI_PSO> EI_Device::CreateGraphicsPSO(const char* vertexShaderName, const char* /*vertexEntryPoint*/, const char* fragmentShaderName, const char* /*fragmentEntryPoint*/, EI_PSOParams& psoParams) {
    RenderingDevice* rd = get_rd();
    if (!rd) {
        UtilityFunctions::push_warning("EI_Device::CreateGraphicsPSO: no RenderingDevice");
        return std::make_unique<EI_PSO>();
    }

    // NOTE: Godot's RD compiles per-stage sources, so we accept GLSL files for each stage.
    // Entry points are not used by Vulkan GLSL (must be main), so our convention is:
    //   <base>.<entry>.<stage>.glsl, where the file defines a main().
    const String v_glsl = make_glsl_path(vertexShaderName, nullptr, "vert");
    const String f_glsl = make_glsl_path(fragmentShaderName, nullptr, "frag");
    const String v_spv = make_spirv_path(vertexShaderName, "vert");
    const String f_spv = make_spirv_path(fragmentShaderName, "frag");

    if ((v_glsl.is_empty() && v_spv.is_empty()) || (f_glsl.is_empty() && f_spv.is_empty())) {
        UtilityFunctions::push_warning("EI_Device::CreateGraphicsPSO: empty shader names");
        return std::make_unique<EI_PSO>();
    }

    Ref<RDShaderSPIRV> spirv;
    spirv.instantiate();

    // Prefer SPIR-V, fallback to GLSL compilation.
    {
        const PackedByteArray vbc = load_file_bytes_or_empty(v_spv);
        if (!vbc.is_empty()) {
            spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX, vbc);
        } else {
            Ref<RDShaderSPIRV> v_stage = compile_spirv_from_glsl(rd, RenderingDevice::SHADER_STAGE_VERTEX, v_glsl, String(vertexShaderName ? vertexShaderName : "vs"));
            if (v_stage.is_valid()) {
                spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX, v_stage->get_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX));
            }
        }
    }
    {
        const PackedByteArray fbc = load_file_bytes_or_empty(f_spv);
        if (!fbc.is_empty()) {
            spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT, fbc);
        } else {
            Ref<RDShaderSPIRV> f_stage = compile_spirv_from_glsl(rd, RenderingDevice::SHADER_STAGE_FRAGMENT, f_glsl, String(fragmentShaderName ? fragmentShaderName : "fs"));
            if (f_stage.is_valid()) {
                spirv->set_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT, f_stage->get_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT));
            }
        }
    }

    if (spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_VERTEX).is_empty() || spirv->get_stage_bytecode(RenderingDevice::SHADER_STAGE_FRAGMENT).is_empty()) {
        UtilityFunctions::push_warning(String("EI_Device::CreateGraphicsPSO: missing SPIR-V and GLSL fallback. vert=") + v_spv + String("/") + v_glsl + String(" frag=") + f_spv + String("/") + f_glsl);
        return std::make_unique<EI_PSO>();
    }

    RID shader = rd->shader_create_from_spirv(spirv, String("tressfx_gfx_") + String(vertexShaderName ? vertexShaderName : "vs"));
    if (!shader.is_valid()) {
        UtilityFunctions::push_warning(String("EI_Device::CreateGraphicsPSO: shader_create_from_spirv failed: ") + String(vertexShaderName ? vertexShaderName : "vs"));
        return std::make_unique<EI_PSO>();
    }

    // Minimal vertex format: location0 = vec3 position.
    Ref<RDVertexAttribute> a0;
    a0.instantiate();
    a0->set_location(0);
    a0->set_offset(0);
    a0->set_format(RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT);
    a0->set_stride(12);
    a0->set_frequency(RenderingDevice::VERTEX_FREQUENCY_VERTEX);

    TypedArray<RDVertexAttribute> attrs;
    attrs.push_back(a0);
    const int64_t vertex_format = rd->vertex_format_create(attrs);

    // Placeholder framebuffer format. When we have a real RenderTargetSet integration,
    // we will create pipelines against that framebuffer format.
    const int64_t fb_format = rd->framebuffer_format_create_empty(RenderingDevice::TEXTURE_SAMPLES_1);

    Ref<RDPipelineRasterizationState> rast;
    rast.instantiate();
    rast->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
    rast->set_front_face(RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
    rast->set_line_width(1.0f);

    Ref<RDPipelineMultisampleState> ms;
    ms.instantiate();
    ms->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_1);

    Ref<RDPipelineDepthStencilState> ds;
    ds.instantiate();
    ds->set_enable_depth_test(psoParams.depthTestEnable);
    ds->set_enable_depth_write(psoParams.depthWriteEnable);
    // NOTE: compare op mapping can be expanded later; default is ALWAYS.
    ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_ALWAYS);

    Ref<RDPipelineColorBlendStateAttachment> att;
    att.instantiate();
    att->set_write_r(true);
    att->set_write_g(true);
    att->set_write_b(true);
    att->set_write_a(true);
    att->set_enable_blend(psoParams.colorBlendParams.colorBlendEnabled);
    if (psoParams.colorBlendParams.colorBlendEnabled) {
        att->set_src_color_blend_factor(map_blend_factor(psoParams.colorBlendParams.colorSrcBlend));
        att->set_dst_color_blend_factor(map_blend_factor(psoParams.colorBlendParams.colorDstBlend));
        att->set_color_blend_op(map_blend_op(psoParams.colorBlendParams.colorBlendOp));
        att->set_src_alpha_blend_factor(map_blend_factor(psoParams.colorBlendParams.alphaSrcBlend));
        att->set_dst_alpha_blend_factor(map_blend_factor(psoParams.colorBlendParams.alphaDstBlend));
        att->set_alpha_blend_op(map_blend_op(psoParams.colorBlendParams.alphaBlendOp));
    }

    TypedArray<RDPipelineColorBlendStateAttachment> atts;
    atts.push_back(att);

    Ref<RDPipelineColorBlendState> blend;
    blend.instantiate();
    blend->set_attachments(atts);

    const RenderingDevice::RenderPrimitive prim = map_primitive(psoParams.primitiveTopology);
    RID pipeline = rd->render_pipeline_create(shader, fb_format, vertex_format, prim, rast, ms, ds, blend);
    if (!pipeline.is_valid()) {
        UtilityFunctions::push_warning("EI_Device::CreateGraphicsPSO: render_pipeline_create failed (placeholder framebuffer format?)");
        rd->free_rid(shader);
        return std::make_unique<EI_PSO>();
    }

    auto pso = std::make_unique<EI_PSO>();
    pso->rd = rd;
    pso->m_bp = EI_BP_GRAPHICS;
    pso->shader = shader;
    pso->pipeline = pipeline;
    return pso;
}

void EI_Device::FlushGPU() {
    RenderingDevice* rd = get_rd();
    if (!rd) {
        return;
    }
    rd->submit();
    rd->sync();
}

void EI_Device::EndAndSubmitCommandBuffer() {
    RenderingDevice* rd = get_rd();
    m_currentCommandBuffer.set_rd(rd);
    m_currentCommandBuffer.EndAndSubmit();
}

static uint32_t read_u32_le(const PackedByteArray& bytes) {
    if (bytes.size() < 4) {
        return 0;
    }
    const uint8_t* p = bytes.ptr();
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct MainRDTestState {
    godot::RenderingDevice* rd = nullptr;
    godot::RID buffer;
    godot::RID shader;
    godot::RID pipeline;
    godot::RID uniform_set;
    bool active = false;
};

static MainRDTestState* g_main_rd_test = nullptr;

static MainRDTestState& get_main_rd_test_state() {
    // Lazy allocation to avoid any risk of non-trivial Godot type initialization at DLL load.
    if (!g_main_rd_test) {
        g_main_rd_test = new MainRDTestState();
    }
    return *g_main_rd_test;
}

static void cleanup_main_rd_test_on_render_thread() {
    MainRDTestState& st = get_main_rd_test_state();
    if (!st.active || !st.rd) {
        * g_main_rd_test = MainRDTestState{};
        return;
    }

    RenderingDevice* rd = st.rd;
    if (st.uniform_set.is_valid()) {
        rd->free_rid(st.uniform_set);
    }
    if (st.pipeline.is_valid()) {
        rd->free_rid(st.pipeline);
    }
    if (st.shader.is_valid()) {
        rd->free_rid(st.shader);
    }
    if (st.buffer.is_valid()) {
        rd->free_rid(st.buffer);
    }

    * g_main_rd_test = MainRDTestState{};
}

static void main_rd_readback_complete(PackedByteArray data) {
    const uint32_t v = read_u32_le(data);
    UtilityFunctions::print(String("EI_Device main-RD self-test: buffer value=") + String::num_int64(v));
    UtilityFunctions::print("EI_Device main-RD self-test: done");

    // Cleanup on render thread.
    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs) {
        rs->call_on_render_thread(callable_mp_static(&cleanup_main_rd_test_on_render_thread));
    }
}

static void run_main_rd_self_test_on_render_thread() {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        UtilityFunctions::push_warning("EI_Device main-RD self-test: RenderingServer is null");
        return;
    }

    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd) {
        UtilityFunctions::push_warning("EI_Device main-RD self-test: get_rendering_device() returned null");
        return;
    }

    UtilityFunctions::print("EI_Device main-RD self-test: begin (render thread)");

    // Ensure previous attempt is cleaned up.
    cleanup_main_rd_test_on_render_thread();
    MainRDTestState& st = get_main_rd_test_state();

    const uint32_t initial_value = 0;
    PackedByteArray init;
    init.resize(4);
    std::memcpy(init.ptrw(), &initial_value, 4);
    st.rd = rd;
    st.buffer = rd->storage_buffer_create(4, init);
    rd->set_resource_name(st.buffer, "tressfx_main_rd_self_test_buffer");

    const String glsl =
        "#version 450\n"
        "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
        "layout(set = 0, binding = 0, std430) buffer Data { uint value; } data;\n"
        "void main() { data.value = 456u; }\n";

    Ref<RDShaderSource> source;
    source.instantiate();
    source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, glsl);

    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source, /*allow_cache=*/true);
    const String err = spirv.is_valid() ? spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE) : String("<no spirv>");
    if (!err.is_empty()) {
        UtilityFunctions::push_warning(String("EI_Device main-RD self-test: shader compile error: ") + err);
        cleanup_main_rd_test_on_render_thread();
        return;
    }

    st.shader = rd->shader_create_from_spirv(spirv, "tressfx_main_rd_self_test_shader");
    if (!st.shader.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD self-test: failed to create shader");
        cleanup_main_rd_test_on_render_thread();
        return;
    }

    st.pipeline = rd->compute_pipeline_create(st.shader);
    if (!st.pipeline.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD self-test: failed to create compute pipeline");
        cleanup_main_rd_test_on_render_thread();
        return;
    }

    Ref<RDUniform> u;
    u.instantiate();
    u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
    u->set_binding(0);
    u->add_id(st.buffer);

    TypedArray<RDUniform> uniforms;
    uniforms.push_back(u);
    st.uniform_set = rd->uniform_set_create(uniforms, st.shader, 0);
    if (!st.uniform_set.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD self-test: failed to create uniform set");
        cleanup_main_rd_test_on_render_thread();
        return;
    }

    const int64_t cl = rd->compute_list_begin();
    rd->compute_list_bind_compute_pipeline(cl, st.pipeline);
    rd->compute_list_bind_uniform_set(cl, st.uniform_set, 0);
    rd->compute_list_dispatch(cl, 1, 1, 1);
    rd->compute_list_add_barrier(cl);
    rd->compute_list_end();

    // IMPORTANT: The main RenderingDevice is not a "local" device. Godot will error if we call submit/sync.
    // Instead, request async readback; the engine will schedule it and invoke the callback.
    st.active = true;
    rd->buffer_get_data_async(st.buffer, callable_mp_static(&main_rd_readback_complete), 0, 4);
}

struct MainRDImageTestState {
    godot::RenderingDevice* rd = nullptr;
    godot::RID texture;
    godot::RID shader;
    godot::RID pipeline;
    godot::RID uniform_set;
    bool active = false;
};

static MainRDImageTestState* g_main_rd_image_test = nullptr;

static MainRDImageTestState& get_main_rd_image_test_state() {
    if (!g_main_rd_image_test) {
        g_main_rd_image_test = new MainRDImageTestState();
    }
    return *g_main_rd_image_test;
}

static void cleanup_main_rd_image_test_on_render_thread() {
    MainRDImageTestState& st = get_main_rd_image_test_state();
    if (!st.active || !st.rd) {
        *g_main_rd_image_test = MainRDImageTestState{};
        return;
    }

    RenderingDevice* rd = st.rd;
    if (st.uniform_set.is_valid()) {
        rd->free_rid(st.uniform_set);
    }
    if (st.pipeline.is_valid()) {
        rd->free_rid(st.pipeline);
    }
    if (st.shader.is_valid()) {
        rd->free_rid(st.shader);
    }
    if (st.texture.is_valid()) {
        rd->free_rid(st.texture);
    }

    *g_main_rd_image_test = MainRDImageTestState{};
}

static void main_rd_image_readback_complete(PackedByteArray data) {
    const uint32_t v = read_u32_le(data);
    UtilityFunctions::print(String("EI_Device main-RD image self-test: tex value=") + String::num_int64(v));
    UtilityFunctions::print("EI_Device main-RD image self-test: done");

    RenderingServer* rs = RenderingServer::get_singleton();
    if (rs) {
        rs->call_on_render_thread(callable_mp_static(&cleanup_main_rd_image_test_on_render_thread));
    }
}

static void run_main_rd_image_self_test_on_render_thread() {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: RenderingServer is null");
        return;
    }

    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: get_rendering_device() returned null");
        return;
    }

    UtilityFunctions::print("EI_Device main-RD image self-test: begin (render thread)");

    cleanup_main_rd_image_test_on_render_thread();
    MainRDImageTestState& st = get_main_rd_image_test_state();
    st.rd = rd;

    // Create a 1x1 R32_UINT storage texture.
    Ref<RDTextureFormat> fmt;
    fmt.instantiate();
    fmt->set_width(1);
    fmt->set_height(1);
    fmt->set_depth(1);
    fmt->set_array_layers(1);
    fmt->set_mipmaps(1);
    fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
    fmt->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
    fmt->set_format(RenderingDevice::DATA_FORMAT_R32_UINT);

    BitField<RenderingDevice::TextureUsageBits> usage =
        RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
        RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
        RenderingDevice::TEXTURE_USAGE_CPU_READ_BIT;

    if (!rd->texture_is_format_supported_for_usage(RenderingDevice::DATA_FORMAT_R32_UINT, usage)) {
        // Some drivers/backends may not accept CPU_READ for this format; try without it.
        usage =
            RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
            RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
            RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
            RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
    }
    fmt->set_usage_bits(usage);

    Ref<RDTextureView> view;
    view.instantiate();

    st.texture = rd->texture_create(fmt, view);
    if (!st.texture.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: failed to create texture");
        cleanup_main_rd_image_test_on_render_thread();
        return;
    }
    rd->set_resource_name(st.texture, "tressfx_main_rd_self_test_r32ui");

    // Initialize texture contents to 0.
    PackedByteArray init;
    init.resize(4);
    const uint32_t initial_value = 0;
    std::memcpy(init.ptrw(), &initial_value, 4);
    rd->texture_update(st.texture, 0, init);

    // Compute shader that writes 789 into the r32ui image.
    const String glsl =
        "#version 450\n"
        "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
        "layout(set = 0, binding = 0, r32ui) uniform writeonly uimage2D img;\n"
        "void main() { imageStore(img, ivec2(0, 0), uvec4(789u, 0u, 0u, 0u)); }\n";

    Ref<RDShaderSource> source;
    source.instantiate();
    source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, glsl);

    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source, /*allow_cache=*/true);
    const String err = spirv.is_valid() ? spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE) : String("<no spirv>");
    if (!err.is_empty()) {
        UtilityFunctions::push_warning(String("EI_Device main-RD image self-test: shader compile error: ") + err);
        cleanup_main_rd_image_test_on_render_thread();
        return;
    }

    st.shader = rd->shader_create_from_spirv(spirv, "tressfx_main_rd_image_self_test_shader");
    if (!st.shader.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: failed to create shader");
        cleanup_main_rd_image_test_on_render_thread();
        return;
    }

    st.pipeline = rd->compute_pipeline_create(st.shader);
    if (!st.pipeline.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: failed to create compute pipeline");
        cleanup_main_rd_image_test_on_render_thread();
        return;
    }

    Ref<RDUniform> u;
    u.instantiate();
    u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
    u->set_binding(0);
    u->add_id(st.texture);

    TypedArray<RDUniform> uniforms;
    uniforms.push_back(u);
    st.uniform_set = rd->uniform_set_create(uniforms, st.shader, 0);
    if (!st.uniform_set.is_valid()) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: failed to create uniform set");
        cleanup_main_rd_image_test_on_render_thread();
        return;
    }

    const int64_t cl = rd->compute_list_begin();
    rd->compute_list_bind_compute_pipeline(cl, st.pipeline);
    rd->compute_list_bind_uniform_set(cl, st.uniform_set, 0);
    rd->compute_list_dispatch(cl, 1, 1, 1);
    rd->compute_list_add_barrier(cl);
    rd->compute_list_end();

    // Read back layer 0. IMPORTANT: do not submit/sync on main RD.
    st.active = true;
    rd->texture_get_data_async(st.texture, 0, callable_mp_static(&main_rd_image_readback_complete));
}

void EI_Device::RunSelfTestOnce() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;

    RenderingDevice* rd = get_rd();
    if (!rd) {
        UtilityFunctions::push_warning("EI_Device self-test: no RenderingDevice");
        return;
    }

    UtilityFunctions::print("EI_Device self-test: begin");

    // 1) Create a small storage buffer (uint).
    const uint32_t initial_value = 0;
    PackedByteArray init;
    init.resize(4);
    std::memcpy(init.ptrw(), &initial_value, 4);
    RID buffer = rd->storage_buffer_create(4, init);
    rd->set_resource_name(buffer, "tressfx_self_test_buffer");

    // 2) Compile a minimal GLSL compute shader.
    const String glsl =
        "#version 450\n"
        "layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;\n"
        "layout(set = 0, binding = 0, std430) buffer Data { uint value; } data;\n"
        "void main() { data.value = 123u; }\n";

    Ref<RDShaderSource> source;
    source.instantiate();
    source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, glsl);

    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source, /*allow_cache=*/true);
    const String err = spirv.is_valid() ? spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE) : String("<no spirv>");
    if (!err.is_empty()) {
        UtilityFunctions::push_warning(String("EI_Device self-test: shader compile error: ") + err);
        if (buffer.is_valid()) {
            rd->free_rid(buffer);
        }
        return;
    }

    RID shader = rd->shader_create_from_spirv(spirv, "tressfx_self_test_shader");
    if (!shader.is_valid()) {
        UtilityFunctions::push_warning("EI_Device self-test: failed to create shader");
        rd->free_rid(buffer);
        return;
    }

    RID pipeline = rd->compute_pipeline_create(shader);
    if (!pipeline.is_valid()) {
        UtilityFunctions::push_warning("EI_Device self-test: failed to create compute pipeline");
        rd->free_rid(shader);
        rd->free_rid(buffer);
        return;
    }

    // 3) Create uniform set for set=0.
    Ref<RDUniform> u;
    u.instantiate();
    u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
    u->set_binding(0);
    u->add_id(buffer);

    TypedArray<RDUniform> uniforms;
    uniforms.push_back(u);
    RID uniform_set = rd->uniform_set_create(uniforms, shader, 0);
    if (!uniform_set.is_valid()) {
        UtilityFunctions::push_warning("EI_Device self-test: failed to create uniform set");
        rd->free_rid(pipeline);
        rd->free_rid(shader);
        rd->free_rid(buffer);
        return;
    }

    // 4) Dispatch.
    const int64_t cl = rd->compute_list_begin();
    rd->compute_list_bind_compute_pipeline(cl, pipeline);
    rd->compute_list_bind_uniform_set(cl, uniform_set, 0);
    rd->compute_list_dispatch(cl, 1, 1, 1);
    rd->compute_list_add_barrier(cl);
    rd->compute_list_end();
    rd->submit();
    rd->sync();

    // 5) Read back.
    const PackedByteArray out = rd->buffer_get_data(buffer, 0, 4);
    const uint32_t v = read_u32_le(out);
    UtilityFunctions::print(String("EI_Device self-test: buffer value=") + String::num_int64(v));

    // Cleanup (self-test only; real resources will live on).
    rd->free_rid(uniform_set);
    rd->free_rid(pipeline);
    rd->free_rid(shader);
    rd->free_rid(buffer);

    UtilityFunctions::print("EI_Device self-test: done");
}

void EI_Device::RunMainRDSelfTestOnce() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;

    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        UtilityFunctions::push_warning("EI_Device main-RD self-test: RenderingServer is null");
        return;
    }

    // Schedule on the render thread to safely use the main RenderingDevice.
    rs->call_on_render_thread(callable_mp_static(&run_main_rd_self_test_on_render_thread));
}

void EI_Device::RunMainRDImageSelfTestOnce() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;

    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        UtilityFunctions::push_warning("EI_Device main-RD image self-test: RenderingServer is null");
        return;
    }

    rs->call_on_render_thread(callable_mp_static(&run_main_rd_image_self_test_on_render_thread));
}

// --- Minimal guide-line render path (main RD) ---------------------------------

struct MainRDGuideLinesState {
    godot::RenderingDevice* rd = nullptr;

    // Source + derived counts.
    int vertices_per_strand = 0;
    int guide_strands = 0;
    int segments = 0;
    int vertex_count = 0;

    // GPU resources.
    godot::RID guide_positions;   // storage buffer (vec4)
    godot::RID line_vertices;     // storage buffer (vec4)
    godot::RID view_ubo;          // uniform buffer (mat4)

    godot::RID compute_shader;
    godot::RID compute_pipeline;
    godot::RID compute_set;

    godot::RID color_tex;
    int64_t fb_format = -1;
    godot::RID framebuffer;

    godot::RID gfx_shader;
    godot::RID gfx_pipeline;
    int64_t vertex_format = -1;
    godot::RID gfx_set;

    uint32_t width = 512;
    uint32_t height = 512;

    bool active = false;
    bool logged_init = false;
};

static MainRDGuideLinesState* g_main_rd_guidelines = nullptr;

static MainRDGuideLinesState& get_main_rd_guidelines_state() {
    if (!g_main_rd_guidelines) {
        g_main_rd_guidelines = new MainRDGuideLinesState();
    }
    return *g_main_rd_guidelines;
}

static void cleanup_main_rd_guidelines_on_render_thread() {
    MainRDGuideLinesState& st = get_main_rd_guidelines_state();
    if (!st.rd) {
        *g_main_rd_guidelines = MainRDGuideLinesState{};
        return;
    }

    godot::RenderingDevice* rd = st.rd;
    if (st.gfx_set.is_valid()) rd->free_rid(st.gfx_set);
    if (st.gfx_pipeline.is_valid()) rd->free_rid(st.gfx_pipeline);
    if (st.gfx_shader.is_valid()) rd->free_rid(st.gfx_shader);
    if (st.framebuffer.is_valid()) rd->free_rid(st.framebuffer);
    if (st.fb_format != -1) {
        // framebuffer formats are int64 ids; no free call.
        st.fb_format = -1;
    }
    if (st.color_tex.is_valid()) rd->free_rid(st.color_tex);
    if (st.compute_set.is_valid()) rd->free_rid(st.compute_set);
    if (st.compute_pipeline.is_valid()) rd->free_rid(st.compute_pipeline);
    if (st.compute_shader.is_valid()) rd->free_rid(st.compute_shader);
    if (st.view_ubo.is_valid()) rd->free_rid(st.view_ubo);
    if (st.line_vertices.is_valid()) rd->free_rid(st.line_vertices);
    if (st.guide_positions.is_valid()) rd->free_rid(st.guide_positions);

    *g_main_rd_guidelines = MainRDGuideLinesState{};
}

static Ref<RDShaderSPIRV> compile_spirv_from_glsl_source(RenderingDevice* rd, const String& vs_path, const String& fs_path) {
    if (!rd) {
        return Ref<RDShaderSPIRV>();
    }
    const String vsrc = load_file_text_or_empty(vs_path);
    const String fsrc = load_file_text_or_empty(fs_path);
    if (vsrc.is_empty() || fsrc.is_empty()) {
        return Ref<RDShaderSPIRV>();
    }
    Ref<RDShaderSource> src;
    src.instantiate();
    src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    src->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX, vsrc);
    src->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT, fsrc);

    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src, true);
    if (!spirv.is_valid()) {
        return Ref<RDShaderSPIRV>();
    }
    const String verr = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX);
    const String ferr = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
    if (!verr.is_empty() || !ferr.is_empty()) {
        UtilityFunctions::push_warning(String("GuideLines shader compile error:\nVS:\n") + verr + String("\nFS:\n") + ferr);
        return Ref<RDShaderSPIRV>();
    }
    return spirv;
}

static Ref<RDShaderSPIRV> compile_spirv_from_glsl_compute(RenderingDevice* rd, const String& cs_path) {
    if (!rd) {
        return Ref<RDShaderSPIRV>();
    }
    const String csrc = load_file_text_or_empty(cs_path);
    if (csrc.is_empty()) {
        return Ref<RDShaderSPIRV>();
    }
    Ref<RDShaderSource> src;
    src.instantiate();
    src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
    src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, csrc);

    Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src, true);
    if (!spirv.is_valid()) {
        return Ref<RDShaderSPIRV>();
    }
    const String err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
    if (!err.is_empty()) {
        UtilityFunctions::push_warning(String("GuideLines compute compile error:\n") + err);
        return Ref<RDShaderSPIRV>();
    }
    return spirv;
}

static void ensure_main_rd_guidelines_resources(RenderingDevice* rd, const PackedByteArray& positions_vec4, const PackedByteArray& viewproj_mat4, int vps, int guide_strands) {
    MainRDGuideLinesState& st = get_main_rd_guidelines_state();

    const bool need_recreate =
        (!st.active) ||
        (st.rd != rd) ||
        (st.vertices_per_strand != vps) ||
        (st.guide_strands != guide_strands) ||
        (!st.guide_positions.is_valid()) ||
        (!st.line_vertices.is_valid()) ||
        (!st.compute_pipeline.is_valid()) ||
        (!st.gfx_pipeline.is_valid()) ||
        (!st.color_tex.is_valid()) ||
        (!st.framebuffer.is_valid());

    if (!need_recreate) {
        return;
    }

    cleanup_main_rd_guidelines_on_render_thread();
    st = MainRDGuideLinesState{};
    st.rd = rd;
    st.vertices_per_strand = vps;
    st.guide_strands = guide_strands;
    st.segments = guide_strands * std::max(0, vps - 1);
    st.vertex_count = st.segments * 2;

    if (st.segments <= 0 || positions_vec4.is_empty()) {
        UtilityFunctions::push_warning("GuideLines: no segments/positions; skipping resource init");
        return;
    }

    // Buffers.
    st.guide_positions = rd->storage_buffer_create((uint32_t)positions_vec4.size(), positions_vec4);
    rd->set_resource_name(st.guide_positions, "tressfx_guidelines_positions");

    const uint32_t out_bytes = (uint32_t)st.vertex_count * 16u; // vec4 per vertex
    st.line_vertices = rd->storage_buffer_create(out_bytes);
    rd->set_resource_name(st.line_vertices, "tressfx_guidelines_line_vertices");

    st.view_ubo = rd->uniform_buffer_create(64, viewproj_mat4);
    rd->set_resource_name(st.view_ubo, "tressfx_guidelines_view_ubo");

    // Compute pipeline.
    {
        const String cs_path = "res://shaders/glsl/TressFXGuideLines.Prepare.comp.glsl";
        Ref<RDShaderSPIRV> spirv = compile_spirv_from_glsl_compute(rd, cs_path);
        if (!spirv.is_valid()) {
            UtilityFunctions::push_warning(String("GuideLines: missing/failed compute shader: ") + cs_path);
            return;
        }
        st.compute_shader = rd->shader_create_from_spirv(spirv, "tressfx_guidelines_prepare_shader");
        st.compute_pipeline = rd->compute_pipeline_create(st.compute_shader);
        if (!st.compute_pipeline.is_valid()) {
            UtilityFunctions::push_warning("GuideLines: compute_pipeline_create failed");
            return;
        }

        Ref<RDUniform> u0;
        u0.instantiate();
        u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
        u0->set_binding(0);
        u0->add_id(st.guide_positions);

        Ref<RDUniform> u1;
        u1.instantiate();
        u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
        u1->set_binding(1);
        u1->add_id(st.line_vertices);

        TypedArray<RDUniform> uniforms;
        uniforms.push_back(u0);
        uniforms.push_back(u1);
        st.compute_set = rd->uniform_set_create(uniforms, st.compute_shader, 0);
        if (!st.compute_set.is_valid()) {
            UtilityFunctions::push_warning("GuideLines: uniform_set_create failed (compute)");
            return;
        }
    }

    // Offscreen texture + framebuffer.
    {
        Ref<RDTextureFormat> fmt;
        fmt.instantiate();
        fmt->set_width(st.width);
        fmt->set_height(st.height);
        fmt->set_depth(1);
        fmt->set_array_layers(1);
        fmt->set_mipmaps(1);
        fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
        fmt->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
        fmt->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
        fmt->set_usage_bits(
            RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
            RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
            RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
            RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);

        Ref<RDTextureView> view;
        view.instantiate();
        st.color_tex = rd->texture_create(fmt, view);
        rd->set_resource_name(st.color_tex, "tressfx_guidelines_color");

        Ref<RDAttachmentFormat> a;
        a.instantiate();
        a->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
        a->set_samples(RenderingDevice::TEXTURE_SAMPLES_1);
        a->set_usage_flags((uint32_t)RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT);

        TypedArray<RDAttachmentFormat> atts;
        atts.push_back(a);
        st.fb_format = rd->framebuffer_format_create(atts);

        TypedArray<RID> texs;
        texs.push_back(st.color_tex);
        st.framebuffer = rd->framebuffer_create(texs, st.fb_format);
        if (!st.framebuffer.is_valid()) {
            UtilityFunctions::push_warning("GuideLines: framebuffer_create failed");
            return;
        }
    }

    // Graphics pipeline.
    {
        const String vs_path = "res://shaders/glsl/tressfx_guidelines.vert.glsl";
        const String fs_path = "res://shaders/glsl/tressfx_guidelines.frag.glsl";
        Ref<RDShaderSPIRV> spirv = compile_spirv_from_glsl_source(rd, vs_path, fs_path);
        if (!spirv.is_valid()) {
            UtilityFunctions::push_warning(String("GuideLines: missing/failed graphics shaders: ") + vs_path + String(" / ") + fs_path);
            return;
        }
        st.gfx_shader = rd->shader_create_from_spirv(spirv, "tressfx_guidelines_gfx_shader");
        if (!st.gfx_shader.is_valid()) {
            UtilityFunctions::push_warning("GuideLines: shader_create_from_spirv failed (gfx)");
            return;
        }

        // Vertex shader uses gl_VertexIndex + storage buffer fetch, so vertex format can be empty.
        TypedArray<RDVertexAttribute> attrs;
        st.vertex_format = rd->vertex_format_create(attrs);
        if (st.vertex_format < 0) {
            UtilityFunctions::push_warning("GuideLines: vertex_format_create failed");
            return;
        }

        Ref<RDPipelineRasterizationState> rast;
        rast.instantiate();
        rast->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
        rast->set_front_face(RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
        rast->set_line_width(1.0f);

        Ref<RDPipelineMultisampleState> ms;
        ms.instantiate();
        ms->set_sample_count(RenderingDevice::TEXTURE_SAMPLES_1);

        Ref<RDPipelineDepthStencilState> ds;
        ds.instantiate();
        ds->set_enable_depth_test(false);
        ds->set_enable_depth_write(false);
        ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_ALWAYS);

        Ref<RDPipelineColorBlendStateAttachment> att;
        att.instantiate();
        att->set_write_r(true);
        att->set_write_g(true);
        att->set_write_b(true);
        att->set_write_a(true);
        att->set_enable_blend(false);

        TypedArray<RDPipelineColorBlendStateAttachment> atts;
        atts.push_back(att);

        Ref<RDPipelineColorBlendState> blend;
        blend.instantiate();
        blend->set_attachments(atts);

        st.gfx_pipeline = rd->render_pipeline_create(
            st.gfx_shader,
            st.fb_format,
            st.vertex_format,
            RenderingDevice::RENDER_PRIMITIVE_LINES,
            rast,
            ms,
            ds,
            blend);

        if (!st.gfx_pipeline.is_valid()) {
            UtilityFunctions::push_warning("GuideLines: render_pipeline_create failed");
            return;
        }

        Ref<RDUniform> b0;
        b0.instantiate();
        b0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
        b0->set_binding(0);
        b0->add_id(st.line_vertices);

        Ref<RDUniform> b1;
        b1.instantiate();
        b1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
        b1->set_binding(1);
        b1->add_id(st.view_ubo);

        TypedArray<RDUniform> uniforms;
        uniforms.push_back(b0);
        uniforms.push_back(b1);
        st.gfx_set = rd->uniform_set_create(uniforms, st.gfx_shader, 0);
        if (!st.gfx_set.is_valid()) {
            UtilityFunctions::push_warning("GuideLines: uniform_set_create failed (gfx)");
            return;
        }
    }

    st.active = true;
    if (!st.logged_init) {
        UtilityFunctions::print(String("GuideLines: initialized on main RD. guides=") + String::num_int64(st.guide_strands) +
                                String(" vps=") + String::num_int64(st.vertices_per_strand) +
                                String(" segments=") + String::num_int64(st.segments) +
                                String(" vertex_count=") + String::num_int64(st.vertex_count) +
                                String(" tex=") + String::num_int64((int64_t)st.width) + String("x") + String::num_int64((int64_t)st.height));
        st.logged_init = true;
    }
}

static void run_main_rd_guidelines_once_on_render_thread() {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        UtilityFunctions::push_warning("GuideLines: RenderingServer is null");
        return;
    }
    RenderingDevice* rd = rs->get_rendering_device();
    if (!rd) {
        UtilityFunctions::push_warning("GuideLines: main RenderingDevice is null");
        return;
    }

    // Pull source from EI_Device.
    EI_Device* dev = GetDevice();
    if (!dev) {
        UtilityFunctions::push_warning("GuideLines: EI_Device is null");
        return;
    }

    PackedByteArray positions;
    PackedByteArray viewproj;
    int vps = 0;
    int guides = 0;
    if (!dev->PopGuideLinesSourceForRenderThread(positions, viewproj, vps, guides)) {
        UtilityFunctions::push_warning("GuideLines: no source positions/matrix provided");
        return;
    }

    ensure_main_rd_guidelines_resources(rd, positions, viewproj, vps, guides);
    MainRDGuideLinesState& st = get_main_rd_guidelines_state();
    if (!st.active) {
        return;
    }

    // Update view UBO (cheap) and positions buffer (only when sizes match; for now we assume stable sizes).
    if (st.view_ubo.is_valid() && viewproj.size() == 64) {
        rd->buffer_update(st.view_ubo, 0, 64, viewproj);
    }
    if (st.guide_positions.is_valid() && positions.size() > 0) {
        rd->buffer_update(st.guide_positions, 0, (uint32_t)positions.size(), positions);
    }

    // Compute: build line vertex buffer from guide positions.
    const int64_t cl = rd->compute_list_begin();
    rd->compute_list_bind_compute_pipeline(cl, st.compute_pipeline);
    rd->compute_list_bind_uniform_set(cl, st.compute_set, 0);

    // Push constants: uvec4(vps, segments, 0, 0)
    PackedByteArray pc;
    pc.resize(16);
    uint32_t* pcu = reinterpret_cast<uint32_t*>(pc.ptrw());
    pcu[0] = (uint32_t)st.vertices_per_strand;
    pcu[1] = (uint32_t)st.segments;
    pcu[2] = 0;
    pcu[3] = 0;
    rd->compute_list_set_push_constant(cl, pc, 16);

    const uint32_t local_size = 64;
    const uint32_t groups = (uint32_t)((st.segments + (int)local_size - 1) / (int)local_size);
    rd->compute_list_dispatch(cl, groups, 1, 1);
    rd->compute_list_add_barrier(cl);
    rd->compute_list_end();

    // Draw to offscreen texture.
    PackedColorArray clears;
    clears.push_back(Color(0, 0, 0, 1));
    const int64_t dl = rd->draw_list_begin(
        st.framebuffer,
        RenderingDevice::DRAW_CLEAR_COLOR_ALL,
        clears,
        1.0f,
        0,
        Rect2(0, 0, (float)st.width, (float)st.height));

    rd->draw_list_bind_render_pipeline(dl, st.gfx_pipeline);
    rd->draw_list_bind_uniform_set(dl, st.gfx_set, 0);
    rd->draw_list_draw(dl, /*use_indices=*/false, /*instances=*/1, /*procedural_vertex_count=*/(uint32_t)st.vertex_count);
    rd->draw_list_end();
}

bool EI_Device::PopGuideLinesSourceForRenderThread(godot::PackedByteArray& out_positions_vec4, godot::PackedByteArray& out_viewproj_mat4, int& out_vertices_per_strand, int& out_guide_strands) {
    std::lock_guard<std::mutex> lock(m_guidelines_mutex);
    if (m_guidelines_positions_vec4.is_empty() || m_guidelines_viewproj_mat4.is_empty() || m_guidelines_vertices_per_strand <= 0 || m_guidelines_guide_strands <= 0) {
        return false;
    }
    out_positions_vec4 = m_guidelines_positions_vec4;
    out_viewproj_mat4 = m_guidelines_viewproj_mat4;
    out_vertices_per_strand = m_guidelines_vertices_per_strand;
    out_guide_strands = m_guidelines_guide_strands;
    m_guidelines_dirty = false;
    return true;
}

void EI_Device::SetGuideLinesSource(const godot::PackedByteArray& guide_positions_vec4, int vertices_per_strand, int guide_strands) {
    if (vertices_per_strand <= 0 || guide_strands <= 0 || guide_positions_vec4.is_empty()) {
        UtilityFunctions::push_warning("EI_Device::SetGuideLinesSource: invalid args");
        return;
    }

    // Compute a simple 2D fit matrix mapping the guide AABB into clip-space.
    // Matrix is column-major (GLSL default).
    float minx = 1e30f, miny = 1e30f;
    float maxx = -1e30f, maxy = -1e30f;
    const int pos_count = guide_strands * vertices_per_strand;
    if (guide_positions_vec4.size() < pos_count * 16) {
        UtilityFunctions::push_warning("EI_Device::SetGuideLinesSource: buffer too small for vps/guides");
        return;
    }

    const float* src = reinterpret_cast<const float*>(guide_positions_vec4.ptr());
    for (int i = 0; i < pos_count; ++i) {
        const float x = src[i * 4 + 0];
        const float y = src[i * 4 + 1];
        minx = std::min(minx, x);
        miny = std::min(miny, y);
        maxx = std::max(maxx, x);
        maxy = std::max(maxy, y);
    }

    const float cx = 0.5f * (minx + maxx);
    const float cy = 0.5f * (miny + maxy);
    float sx = std::max(1e-4f, (maxx - minx));
    float sy = std::max(1e-4f, (maxy - miny));
    // 10% padding.
    sx *= 1.1f;
    sy *= 1.1f;

    const float inv_sx = 2.0f / sx;
    const float inv_sy = 2.0f / sy;
    const float tx = -cx * inv_sx;
    const float ty = -cy * inv_sy;

    PackedByteArray mat;
    mat.resize(64);
    float* m = reinterpret_cast<float*>(mat.ptrw());
    // Column-major 4x4.
    m[0] = inv_sx; m[1] = 0.0f;  m[2] = 0.0f; m[3] = 0.0f;
    m[4] = 0.0f;  m[5] = inv_sy; m[6] = 0.0f; m[7] = 0.0f;
    m[8] = 0.0f;  m[9] = 0.0f;  m[10] = 0.0f; m[11] = 0.0f;
    m[12] = tx;   m[13] = ty;   m[14] = 0.0f; m[15] = 1.0f;

    {
        std::lock_guard<std::mutex> lock(m_guidelines_mutex);
        m_guidelines_positions_vec4 = guide_positions_vec4;
        m_guidelines_viewproj_mat4 = mat;
        m_guidelines_vertices_per_strand = vertices_per_strand;
        m_guidelines_guide_strands = guide_strands;
        m_guidelines_dirty = true;
    }
}

void EI_Device::RunMainRDGuideLinesOnce() {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!rs) {
        UtilityFunctions::push_warning("EI_Device::RunMainRDGuideLinesOnce: RenderingServer is null");
        return;
    }
    rs->call_on_render_thread(callable_mp_static(&run_main_rd_guidelines_once_on_render_thread));
}

godot::RID EI_Device::GetMainRDGuideLinesTextureRID() const {
    MainRDGuideLinesState& st = get_main_rd_guidelines_state();
    return st.color_tex;
}
