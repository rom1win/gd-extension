#pragma once

// Godot-backed EngineInterface implementation (compute-first).
// Keep this focused: implement only the pieces we need to bring up GPU compute.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "TressFX/TressFXCommon.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rd_vertex_attribute.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

typedef int EI_ResourceFormat;

class EI_BindSet {
public:
    EI_BindSet() = default;
    ~EI_BindSet();

    godot::RID rid;
    uint32_t set_index = 0;
    godot::RenderingDevice* rd = nullptr;
    uint64_t rd_instance_id = 0;

    // Godot ties uniform sets to a specific shader RID. TressFX creates bind sets
    // before PSOs/shaders are available, so we store the RDUniform list and create
    // the actual uniform set lazily when a PSO is bound.
    godot::TypedArray<godot::RDUniform> uniforms;
};

class EI_PSO {
public:
    EI_PSO() = default;
    ~EI_PSO();

    EI_BindPoint m_bp = EI_BP_COMPUTE;
    godot::RID shader;
    godot::RID pipeline;
    godot::RenderingDevice* rd = nullptr;
    uint64_t rd_instance_id = 0;
};

class EI_CommandContext {
public:
    EI_CommandContext() = default;

    void set_rd(godot::RenderingDevice* p_rd);

    void SubmitBarrier(int numBarriers, EI_Barrier* barriers);
    void BindPSO(EI_PSO* pso);
    void BindSets(EI_PSO* pso, int numBindSets, EI_BindSet** bindSets);
    void Dispatch(int numGroups);
    void UpdateBuffer(EI_Resource* res, void* data);
    void ClearUint32Image(EI_Resource* res, uint32_t value);
    void ClearFloat32Image(EI_Resource* res, float value);
    void DrawIndexedInstanced(EI_PSO& pso, EI_IndexedDrawParams& drawParams);
    void DrawInstanced(EI_PSO& pso, EI_DrawParams& drawParams);
    void PushConstants(EI_PSO* pso, int size, void* data);

    // Godot RD command list management
    void BeginComputeIfNeeded();
    // Ends an open compute list (if any) and submits queued lists.
    // Returns true iff a submit occurred.
    bool EndAndSubmit();

private:
    godot::RenderingDevice* rd = nullptr;
    int64_t compute_list = -1;
    EI_PSO* bound_pso = nullptr;
    bool has_queued_lists = false;
    // True when rd is the engine's main RenderingDevice: submit()/sync() forbidden.
    bool is_main_rd = false;
};

class EI_Marker
{
public:
    EI_Marker(EI_CommandContext& ctx, const char * string) : m_ctx(ctx) {}
    ~EI_Marker() {}
private:
    EI_CommandContext& m_ctx;
};

class EI_Resource {
public:
    EI_Resource() = default;
    ~EI_Resource();

    int GetHeight() const { return height; }
    int GetWidth() const { return width; }

    godot::RID rid;
    uint32_t size_bytes = 0;
    int width = 0;
    int height = 0;

    EI_ResourceType m_ResourceType = EI_ResourceType::Undefined;
    godot::RenderingDevice* rd = nullptr;
    uint64_t rd_instance_id = 0;
};

struct EI_BindLayout {
    ~EI_BindLayout() = default;
    EI_LayoutDescription description;

    // In Vulkan/DX12 this is implicit by pipeline layout order.
    // We assign it when a PSO is created and layouts are provided.
    int set_index = -1;
};

struct EI_RenderTargetSet
{
    ~EI_RenderTargetSet() {}
    // void SetResources(const EI_Resource** pResourcesArray) {}
};

class EI_GLTFTexturesAndBuffers {};
class EI_GltfPbrPass {};
class EI_GltfDepthPass {};
class GLTFCommon;

class EI_Device
{
public:
    EI_Device();
    ~EI_Device();

    EI_CommandContext& GetCurrentCommandContext() { return m_currentCommandBuffer; }
    
    std::unique_ptr<EI_Resource> CreateBufferResource(const int structSize, const int structCount, const unsigned int flags, const char* name);
    std::unique_ptr<EI_Resource> CreateUint32Resource(const int width, const int height, const size_t arraySize, const char* name, uint32_t ClearValue = 0);
    std::unique_ptr<EI_Resource> CreateRenderTargetResource(const int width, const int height, const size_t channels, const size_t channelSize, const char* name, AMD::float4* ClearValues = nullptr);
    std::unique_ptr<EI_Resource> CreateDepthResource(const int width, const int height, const char* name);
    std::unique_ptr<EI_Resource> CreateResourceFromFile(const char* szFilename, bool useSRGB = false);
    std::unique_ptr<EI_Resource> CreateSampler(EI_Filter MinFilter, EI_Filter MaxFilter, EI_Filter MipFilter, EI_AddressMode AddressMode);
    
    std::unique_ptr<EI_BindLayout> CreateLayout(const EI_LayoutDescription& description);

    std::unique_ptr<EI_BindSet> CreateBindSet(EI_BindLayout* layout, EI_BindSetDescription& bindSet);

    std::unique_ptr<EI_RenderTargetSet> CreateRenderTargetSet(const EI_ResourceFormat* pResourceFormats, const uint32_t numResources, const EI_AttachmentParams* AttachmentParams, float* clearValues) { return std::make_unique<EI_RenderTargetSet>(); }
    std::unique_ptr<EI_RenderTargetSet> CreateRenderTargetSet(const EI_Resource** pResourcesArray, const uint32_t numResources, const EI_AttachmentParams* AttachmentParams, float* clearValues) { return std::make_unique<EI_RenderTargetSet>(); }

    std::unique_ptr<EI_GLTFTexturesAndBuffers> CreateGLTFTexturesAndBuffers(GLTFCommon* pGLTFCommon) { return std::make_unique<EI_GLTFTexturesAndBuffers>(); }
    std::unique_ptr<EI_GltfPbrPass> CreateGLTFPbrPass(EI_GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers, EI_RenderTargetSet* renderTargetSet) { return std::make_unique<EI_GltfPbrPass>(); }
    std::unique_ptr<EI_GltfDepthPass> CreateGLTFDepthPass(EI_GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers, EI_RenderTargetSet* renderTargetSet) { return std::make_unique<EI_GltfDepthPass>(); }

    void BeginRenderPass(EI_CommandContext& commandContext, const EI_RenderTargetSet* pRenderPassSet, const wchar_t* pPassName, uint32_t width = 0, uint32_t height = 0);
    void EndRenderPass(EI_CommandContext& commandContext);
    void SetViewportAndScissor(EI_CommandContext& commandContext, uint32_t topX, uint32_t topY, uint32_t width, uint32_t height);

    std::unique_ptr<EI_PSO> CreateComputeShaderPSO(const char* shaderName, const char* entryPoint, EI_BindLayout** layouts, int numLayouts);
    std::unique_ptr<EI_PSO> CreateGraphicsPSO(const char* vertexShaderName, const char* vertexEntryPoint, const char* fragmentShaderName, const char* fragmentEntryPoint, EI_PSOParams& psoParams);

    /* async compute */
    EI_CommandContext& GetComputeCommandContext() { return m_currentCommandBuffer; } // Reuse same for now
    void WaitForCompute() {}
    void SignalComputeStart() {}
    void WaitForLastFrameGraphics() {}
    void SubmitComputeCommandList() {}
    /* /async compute */

    // internals
    void OnCreate(void* hWnd, uint32_t numBackBuffers, bool enableValidation, const char* appName) {}
    void OnResize(uint32_t width, uint32_t height) {}
    void SetVSync(bool vSync) {}
    void FlushGPU();

    // Local RenderingDevice accessor (main-thread only). Used for early simulation bring-up
    // and CPU readback; main-RD work must still be scheduled on the render thread.
    godot::RenderingDevice* GetLocalRenderingDevice() { return get_rd(); }

    // A1: route ALL EI resources/dispatches to the main RenderingDevice.
    // When enabled, every GPU-touching call must run on the render thread.
    void SetUseMainRD(bool p_enable) { m_use_main_rd = p_enable; }
    bool UsingMainRD() const { return m_use_main_rd; }
    void OnDestroy() {}
    
    void OnBeginFrame(bool bDoAsync) {}
    void OnEndFrame() {}

    // Convenience: one-shot compute validation of the Godot RenderingDevice backend.
    // Safe to call multiple times; only the first call does work.
    void RunSelfTestOnce();

    // One-shot validation using the *main* RenderingDevice, scheduled on the render thread.
    // This is the supported way to use RenderingServer::get_rendering_device() without thread violations.
    void RunMainRDSelfTestOnce();

    // One-shot validation for texture/image resources on the *main* RenderingDevice.
    // Creates an R32_UINT storage texture, writes a value in a compute shader, and reads it back async.
    void RunMainRDImageSelfTestOnce();

    // Milestone 3-5 (minimal GPU guide-line render):
    // Provide guide positions (vec4 std430) then request an offscreen render on the main RD.
    // The output is a color texture RID (wrap it in Texture2DRD to display).
    void SetGuideLinesSource(const godot::PackedByteArray& guide_positions_vec4, int vertices_per_strand, int guide_strands);
    void RunMainRDGuideLinesOnce();
    godot::RID GetMainRDGuideLinesTextureRID() const;

    // Internal: render-thread task pulls the latest source under a mutex.
    bool PopGuideLinesSourceForRenderThread(godot::PackedByteArray& out_positions_vec4, godot::PackedByteArray& out_viewproj_mat4, int& out_vertices_per_strand, int& out_guide_strands);

    // A2.1: GPU-to-GPU position feed for the ribbon renderer. Copies a hair
    // object's simulated positions buffer (float4 per vertex) into a fixed
    // 512x512 RGBA32F texture on the main RD; no CPU readback. RENDER THREAD
    // ONLY (creates GPU resources lazily on first call). Call once per sim
    // tick, right after the kernel dispatches, so the texture always reflects
    // the latest simulated positions.
    void DispatchPositionTextureCopy(godot::RID positions_buffer_rid, int vertex_count);

    // Safe from any thread: reads a cached RID, does no RenderingDevice work.
    // Invalid until the first DispatchPositionTextureCopy() call has run.
    godot::RID GetPositionTextureRID() const;

    // Minimal submission hook (used by self-test and later by simulation/render integration).
    void EndAndSubmitCommandBuffer();
    
    EI_Resource* GetDepthBufferResource() { return nullptr; }
    EI_ResourceFormat GetDepthBufferFormat() { return 0; }
    EI_Resource* GetColorBufferResource() { return nullptr; }
    EI_ResourceFormat GetColorBufferFormat() { return 0; }
    EI_Resource* GetShadowBufferResource() { return nullptr; }
    EI_ResourceFormat GetShadowBufferFormat() { return 0; }
    EI_Resource* GetDefaultWhiteTexture();
    EI_BindSet* GetSamplerBindSet() { return nullptr; }

    void GetTimeStamp(char * name) {}
    int GetNumTimeStamps() { return 0; }
    const char* GetTimeStampName(const int i) { return ""; }
    int GetTimeStampValue(const int i) { return 0; }
    void DrawFullScreenQuad(EI_CommandContext& commandContext, EI_PSO& pso, EI_BindSet** bindSets, uint32_t numBindSets) {}
    float GetAverageGpuTime() const { return 0.0f; }

private:
    godot::RenderingDevice* get_rd();
    godot::RenderingDevice* m_local_rd = nullptr;
    bool m_use_main_rd = false;

    std::unique_ptr<EI_Resource> m_default_white_texture;
    std::unique_ptr<EI_Resource> m_default_linear_sampler;

    EI_CommandContext m_currentCommandBuffer;

    // Local RenderingDevice submission tracking.
    // Godot errors if sync() is called without a prior submit().
    bool m_local_rd_needs_sync = false;

    // Guide-line debug source data (main thread) -> consumed on render thread.
    mutable std::mutex m_guidelines_mutex;
    godot::PackedByteArray m_guidelines_positions_vec4;
    godot::PackedByteArray m_guidelines_viewproj_mat4;
    int m_guidelines_vertices_per_strand = 0;
    int m_guidelines_guide_strands = 0;
    bool m_guidelines_dirty = false;
};

EI_Device * GetDevice();

// NOTE: EI_Device owns Godot Variant types (PackedByteArray, RID, etc) and must not be
// constructed during DLL load (before godot-cpp initializes the interface pointers).
// These helpers are called from the GDExtension init/terminate hooks.
void InitializeGodotEngineInterface();
void ShutdownGodotEngineInterface();
