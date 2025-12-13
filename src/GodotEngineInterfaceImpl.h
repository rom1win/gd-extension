#pragma once

#include <vector>
#include <memory>
#include <string>
#include "TressFX/TressFXCommon.h"

// Stub implementation for Godot
// This file is included by EngineInterface.h, so it has access to EI_* types defined there.

typedef int EI_ResourceFormat;

class EI_BindSet
{
public:
    ~EI_BindSet() {}
};

class EI_PSO
{
public:
    ~EI_PSO() {}
    EI_BindPoint m_bp;
};

class EI_CommandContext
{
public:
    void SubmitBarrier(int numBarriers, EI_Barrier * barriers) {}
    void BindPSO(EI_PSO * pso) {}
    void BindSets(EI_PSO * pso, int numBindSets, EI_BindSet ** bindSets) {}
    void Dispatch(int numGroups) {}
    void UpdateBuffer(EI_Resource * res, void * data) {}
    void ClearUint32Image(EI_Resource* res, uint32_t value) {}
    void ClearFloat32Image(EI_Resource* res, float value) {}
    void DrawIndexedInstanced(EI_PSO& pso, EI_IndexedDrawParams& drawParams) {}
    void DrawInstanced(EI_PSO& pso, EI_DrawParams& drawParams) {}
    void PushConstants(EI_PSO * pso, int size, void * data) {}
};

class EI_Marker
{
public:
    EI_Marker(EI_CommandContext& ctx, const char * string) : m_ctx(ctx) {}
    ~EI_Marker() {}
private:
    EI_CommandContext& m_ctx;
};

class EI_Resource
{
public:
    EI_Resource() {}
    ~EI_Resource() {}

    int GetHeight() const { return 0; }
    int GetWidth() const { return 0; }

    EI_ResourceType m_ResourceType = EI_ResourceType::Undefined;
};

struct EI_BindLayout
{
    ~EI_BindLayout() {}
    EI_LayoutDescription description;
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
    EI_Device() {}
    ~EI_Device() {}

    EI_CommandContext& GetCurrentCommandContext() { return m_currentCommandBuffer; }
    
    std::unique_ptr<EI_Resource> CreateBufferResource(const int structSize, const int structCount, const unsigned int flags, const char* name) { return std::make_unique<EI_Resource>(); }
    std::unique_ptr<EI_Resource> CreateUint32Resource(const int width, const int height, const size_t arraySize, const char* name, uint32_t ClearValue = 0) { return std::make_unique<EI_Resource>(); }
    std::unique_ptr<EI_Resource> CreateRenderTargetResource(const int width, const int height, const size_t channels, const size_t channelSize, const char* name, AMD::float4* ClearValues = nullptr) { return std::make_unique<EI_Resource>(); }
    std::unique_ptr<EI_Resource> CreateDepthResource(const int width, const int height, const char* name) { return std::make_unique<EI_Resource>(); }
    std::unique_ptr<EI_Resource> CreateResourceFromFile(const char* szFilename, bool useSRGB = false) { return std::make_unique<EI_Resource>(); }
    std::unique_ptr<EI_Resource> CreateSampler(EI_Filter MinFilter, EI_Filter MaxFilter, EI_Filter MipFilter, EI_AddressMode AddressMode) { return std::make_unique<EI_Resource>(); }
    
    std::unique_ptr<EI_BindLayout> CreateLayout(const EI_LayoutDescription& description) { return std::make_unique<EI_BindLayout>(); }

    std::unique_ptr<EI_BindSet> CreateBindSet(EI_BindLayout * layout, EI_BindSetDescription& bindSet) { return std::make_unique<EI_BindSet>(); }

    std::unique_ptr<EI_RenderTargetSet> CreateRenderTargetSet(const EI_ResourceFormat* pResourceFormats, const uint32_t numResources, const EI_AttachmentParams* AttachmentParams, float* clearValues) { return std::make_unique<EI_RenderTargetSet>(); }
    std::unique_ptr<EI_RenderTargetSet> CreateRenderTargetSet(const EI_Resource** pResourcesArray, const uint32_t numResources, const EI_AttachmentParams* AttachmentParams, float* clearValues) { return std::make_unique<EI_RenderTargetSet>(); }

    std::unique_ptr<EI_GLTFTexturesAndBuffers> CreateGLTFTexturesAndBuffers(GLTFCommon* pGLTFCommon) { return std::make_unique<EI_GLTFTexturesAndBuffers>(); }
    std::unique_ptr<EI_GltfPbrPass> CreateGLTFPbrPass(EI_GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers, EI_RenderTargetSet* renderTargetSet) { return std::make_unique<EI_GltfPbrPass>(); }
    std::unique_ptr<EI_GltfDepthPass> CreateGLTFDepthPass(EI_GLTFTexturesAndBuffers* pGLTFTexturesAndBuffers, EI_RenderTargetSet* renderTargetSet) { return std::make_unique<EI_GltfDepthPass>(); }

    void BeginRenderPass(EI_CommandContext& commandContext, const EI_RenderTargetSet* pRenderPassSet, const wchar_t* pPassName, uint32_t width = 0, uint32_t height = 0) {}
    void EndRenderPass(EI_CommandContext& commandContext) {}
    void SetViewportAndScissor(EI_CommandContext& commandContext, uint32_t topX, uint32_t topY, uint32_t width, uint32_t height) {}

    std::unique_ptr<EI_PSO> CreateComputeShaderPSO(const char * shaderName, const char * entryPoint, EI_BindLayout ** layouts, int numLayouts) { return std::make_unique<EI_PSO>(); }
    std::unique_ptr<EI_PSO> CreateGraphicsPSO(const char * vertexShaderName, const char * vertexEntryPoint, const char * fragmentShaderName, const char * fragmentEntryPoint, EI_PSOParams& psoParams) { return std::make_unique<EI_PSO>(); }

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
    void FlushGPU() {}
    void OnDestroy() {}
    
    void OnBeginFrame(bool bDoAsync) {}
    void OnEndFrame() {}
    
    EI_Resource* GetDepthBufferResource() { return nullptr; }
    EI_ResourceFormat GetDepthBufferFormat() { return 0; }
    EI_Resource* GetColorBufferResource() { return nullptr; }
    EI_ResourceFormat GetColorBufferFormat() { return 0; }
    EI_Resource* GetShadowBufferResource() { return nullptr; }
    EI_ResourceFormat GetShadowBufferFormat() { return 0; }
    EI_Resource* GetDefaultWhiteTexture() { return nullptr; }
    EI_BindSet* GetSamplerBindSet() { return nullptr; }

    void GetTimeStamp(char * name) {}
    int GetNumTimeStamps() { return 0; }
    const char* GetTimeStampName(const int i) { return ""; }
    int GetTimeStampValue(const int i) { return 0; }
    void DrawFullScreenQuad(EI_CommandContext& commandContext, EI_PSO& pso, EI_BindSet** bindSets, uint32_t numBindSets) {}
    float GetAverageGpuTime() const { return 0.0f; }

private:
    EI_CommandContext m_currentCommandBuffer;
};

EI_Device * GetDevice();
