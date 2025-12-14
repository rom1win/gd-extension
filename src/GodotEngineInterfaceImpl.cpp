#include "EngineInterface.h"

#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

using namespace godot;

static EI_Device g_Device;

EI_Device* GetDevice() {
    return &g_Device;
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

void EI_CommandContext::ClearUint32Image(EI_Resource* /*res*/, uint32_t /*value*/) {
    // TODO: implement when image resources are wired.
}

void EI_CommandContext::ClearFloat32Image(EI_Resource* /*res*/, float /*value*/) {
    // TODO: implement when image resources are wired.
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

std::unique_ptr<EI_Resource> EI_Device::CreateUint32Resource(const int /*width*/, const int /*height*/, const size_t /*arraySize*/, const char* /*name*/, uint32_t /*ClearValue*/) {
    // TODO: implement image resources when SDF/render paths are enabled.
    return std::make_unique<EI_Resource>();
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

std::unique_ptr<EI_Resource> EI_Device::CreateSampler(EI_Filter /*MinFilter*/, EI_Filter /*MaxFilter*/, EI_Filter /*MipFilter*/, EI_AddressMode /*AddressMode*/) {
    // TODO: implement when textures are needed.
    return std::make_unique<EI_Resource>();
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

    // NOTE: uniform sets are created against a shader RID, but at this stage we don't have it.
    // The Godot backend will complete this once CreateComputeShaderPSO is wired to real shaders.
    // For now return an empty bind set.
    auto set = std::make_unique<EI_BindSet>();
    set->rd = rd;
    set->set_index = (uint32_t)layout->set_index;
    return set;
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

std::unique_ptr<EI_PSO> EI_Device::CreateComputeShaderPSO(const char* shaderName, const char* entryPoint, EI_BindLayout** layouts, int numLayouts) {
    // Not wired to TressFX shaders yet (needs HLSL->SPIR-V pipeline). Compute self-test is implemented separately.
    (void)entryPoint;
    if (layouts) {
        for (int i = 0; i < numLayouts; ++i) {
            if (layouts[i] && layouts[i]->set_index < 0) {
                layouts[i]->set_index = i;
            }
        }
    }

    UtilityFunctions::push_warning(String("EI_Device::CreateComputeShaderPSO: not implemented for '") + String(shaderName ? shaderName : "(null)") + String("'"));
    return std::make_unique<EI_PSO>();
}

std::unique_ptr<EI_PSO> EI_Device::CreateGraphicsPSO(const char* /*vertexShaderName*/, const char* /*vertexEntryPoint*/, const char* /*fragmentShaderName*/, const char* /*fragmentEntryPoint*/, EI_PSOParams& /*psoParams*/) {
    // Not needed for compute-first milestone.
    return std::make_unique<EI_PSO>();
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
