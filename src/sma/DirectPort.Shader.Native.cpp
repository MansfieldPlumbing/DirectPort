#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgiformat.h>
#include <sddl.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <string>

#include "DirectPort.Shader.Native.h"

using Microsoft::WRL::ComPtr;

namespace {
    constexpr uint32_t kFrameCount = 3;
    thread_local std::string g_error;

    struct FrameSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        uint64_t completion = 0;
    };

    struct ShaderState {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t constantStride = 256;
        uint32_t constantCapacity = 0;
        uint32_t nextSlot = 0;
        uint64_t internalValue = 0;

        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12Resource> target;
        ComPtr<ID3D12Fence> sharedFence;
        ComPtr<ID3D12Fence> internalFence;
        ComPtr<ID3D12DescriptorHeap> rtvHeap;
        ComPtr<ID3D12RootSignature> rootSignature;
        ComPtr<ID3D12PipelineState> pipeline;
        ComPtr<ID3D12Resource> constants;
        uint8_t* mappedConstants = nullptr;
        FrameSlot frames[kFrameCount];

        HANDLE textureHandle = nullptr;
        HANDLE fenceHandle = nullptr;
        HANDLE eventHandle = nullptr;
    };

    DXGI_FORMAT ToFormat(int32_t value) {
        switch (value) {
            case 0: return DXGI_FORMAT_B8G8R8A8_UNORM;
            case 1: return DXGI_FORMAT_R32_FLOAT;
            case 2: return DXGI_FORMAT_R16_FLOAT;
            case 3: return DXGI_FORMAT_R32_UINT;
            default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    bool Failed(HRESULT hr, const char* operation) {
        if (SUCCEEDED(hr)) return false;
        char buffer[256];
        sprintf_s(buffer, "%s failed (HRESULT 0x%08X)", operation, static_cast<unsigned>(hr));
        g_error = buffer;
        return true;
    }

    void SetCompileError(const char* stage, ID3DBlob* errors, HRESULT hr) {
        g_error = stage;
        g_error += " compilation failed (HRESULT 0x";
        char value[16];
        sprintf_s(value, "%08X", static_cast<unsigned>(hr));
        g_error += value;
        g_error += ")";
        if (errors && errors->GetBufferPointer()) {
            g_error += ": ";
            g_error.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        }
    }

    void CloseState(ShaderState* s) {
        if (!s) return;

        if (s->queue && s->internalFence && s->eventHandle) {
            const uint64_t value = ++s->internalValue;
            if (SUCCEEDED(s->queue->Signal(s->internalFence.Get(), value)) &&
                s->internalFence->GetCompletedValue() < value &&
                SUCCEEDED(s->internalFence->SetEventOnCompletion(value, s->eventHandle))) {
                WaitForSingleObject(s->eventHandle, INFINITE);
            }
        }

        if (s->constants && s->mappedConstants)
            s->constants->Unmap(0, nullptr);
        s->mappedConstants = nullptr;
        if (s->textureHandle) CloseHandle(s->textureHandle);
        if (s->fenceHandle) CloseHandle(s->fenceHandle);
        if (s->eventHandle) CloseHandle(s->eventHandle);
        delete s;
    }
}

extern "C" {

DPS_EXPORT DPS_HANDLE dps_create(
    uint32_t width,
    uint32_t height,
    int32_t formatValue,
    const wchar_t* textureName,
    const wchar_t* fenceName,
    const char* hlsl,
    const char* vertexEntry,
    const char* pixelEntry,
    uint32_t constantCapacity) {

    g_error.clear();
    if (!width || !height || !textureName || !fenceName || !hlsl || !vertexEntry || !pixelEntry) {
        g_error = "dps_create received a null or zero required argument";
        return nullptr;
    }

    const DXGI_FORMAT format = ToFormat(formatValue);
    if (format == DXGI_FORMAT_UNKNOWN) {
        g_error = "unsupported output format";
        return nullptr;
    }

    ShaderState* s = new (std::nothrow) ShaderState();
    if (!s) {
        g_error = "out of memory";
        return nullptr;
    }
    s->width = width;
    s->height = height;
    s->constantCapacity = constantCapacity;
    s->constantStride = std::max<uint32_t>(256u, (constantCapacity + 255u) & ~255u);

    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s->device));
    if (Failed(hr, "D3D12CreateDevice")) { CloseState(s); return nullptr; }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&s->queue));
    if (Failed(hr, "CreateCommandQueue")) { CloseState(s); return nullptr; }

    hr = s->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s->sharedFence));
    if (Failed(hr, "Create shared fence")) { CloseState(s); return nullptr; }
    hr = s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->internalFence));
    if (Failed(hr, "Create internal fence")) { CloseState(s); return nullptr; }
    s->eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s->eventHandle) { g_error = "CreateEvent failed"; CloseState(s); return nullptr; }

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr = s->device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_SHARED, &textureDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s->target));
    if (Failed(hr, "Create shared render texture")) { CloseState(s); return nullptr; }

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &securityDescriptor, nullptr)) {
        g_error = "ConvertStringSecurityDescriptorToSecurityDescriptor failed";
        CloseState(s);
        return nullptr;
    }
    SECURITY_ATTRIBUTES security = { sizeof(security), securityDescriptor, FALSE };
    hr = s->device->CreateSharedHandle(s->target.Get(), &security, GENERIC_ALL, textureName, &s->textureHandle);
    if (SUCCEEDED(hr))
        hr = s->device->CreateSharedHandle(s->sharedFence.Get(), &security, GENERIC_ALL, fenceName, &s->fenceHandle);
    LocalFree(securityDescriptor);
    if (Failed(hr, "CreateSharedHandle")) { CloseState(s); return nullptr; }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 1;
    hr = s->device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&s->rtvHeap));
    if (Failed(hr, "Create RTV heap")) { CloseState(s); return nullptr; }
    s->device->CreateRenderTargetView(s->target.Get(), nullptr, s->rtvHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_ROOT_PARAMETER rootParameter = {};
    rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameter.Descriptor.ShaderRegister = 0;
    rootParameter.Descriptor.RegisterSpace = 0;
    rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &rootParameter;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3DBlob> rootError;
    hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &rootError);
    if (FAILED(hr)) { SetCompileError("root signature", rootError.Get(), hr); CloseState(s); return nullptr; }
    hr = s->device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(), IID_PPV_ARGS(&s->rootSignature));
    if (Failed(hr, "CreateRootSignature")) { CloseState(s); return nullptr; }

    ComPtr<ID3DBlob> vertexShader;
    ComPtr<ID3DBlob> pixelShader;
    ComPtr<ID3DBlob> compileError;
    const UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, vertexEntry, "vs_5_0", compileFlags, 0, &vertexShader, &compileError);
    if (FAILED(hr)) { SetCompileError("vertex shader", compileError.Get(), hr); CloseState(s); return nullptr; }
    compileError.Reset();
    hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, pixelEntry, "ps_5_0", compileFlags, 0, &pixelShader, &compileError);
    if (FAILED(hr)) { SetCompileError("pixel shader", compileError.Get(), hr); CloseState(s); return nullptr; }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = s->rootSignature.Get();
    pso.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    pso.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    hr = s->device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&s->pipeline));
    if (Failed(hr, "CreateGraphicsPipelineState")) { CloseState(s); return nullptr; }

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = static_cast<UINT64>(s->constantStride) * kFrameCount;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    hr = s->device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s->constants));
    if (Failed(hr, "Create constant buffer")) { CloseState(s); return nullptr; }
    D3D12_RANGE noRead = { 0, 0 };
    hr = s->constants->Map(0, &noRead, reinterpret_cast<void**>(&s->mappedConstants));
    if (Failed(hr, "Map constant buffer")) { CloseState(s); return nullptr; }
    memset(s->mappedConstants, 0, static_cast<size_t>(s->constantStride) * kFrameCount);

    for (uint32_t i = 0; i < kFrameCount; ++i) {
        hr = s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s->frames[i].allocator));
        if (Failed(hr, "CreateCommandAllocator")) { CloseState(s); return nullptr; }
        hr = s->device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, s->frames[i].allocator.Get(), nullptr,
            IID_PPV_ARGS(&s->frames[i].list));
        if (Failed(hr, "CreateCommandList")) { CloseState(s); return nullptr; }
        s->frames[i].list->Close();
    }

    return s;
}

DPS_EXPORT bool dps_try_draw(DPS_HANDLE handle, const void* constantData, uint32_t constantBytes, uint64_t publishValue) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    if (!s) { g_error = "null shader producer"; return false; }
    if (constantBytes > s->constantCapacity) { g_error = "constant data exceeds configured capacity"; return false; }
    if (constantBytes && !constantData) { g_error = "constant data pointer is null"; return false; }

    FrameSlot& frame = s->frames[s->nextSlot];
    if (frame.completion && s->internalFence->GetCompletedValue() < frame.completion)
        return false;

    uint8_t* destination = s->mappedConstants + static_cast<size_t>(s->nextSlot) * s->constantStride;
    memset(destination, 0, s->constantStride);
    if (constantBytes)
        memcpy(destination, constantData, constantBytes);

    HRESULT hr = frame.allocator->Reset();
    if (Failed(hr, "Reset command allocator")) return false;
    hr = frame.list->Reset(frame.allocator.Get(), s->pipeline.Get());
    if (Failed(hr, "Reset command list")) return false;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s->target.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    frame.list->ResourceBarrier(1, &barrier);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(s->width), static_cast<float>(s->height), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(s->width), static_cast<LONG>(s->height) };
    frame.list->SetGraphicsRootSignature(s->rootSignature.Get());
    frame.list->RSSetViewports(1, &viewport);
    frame.list->RSSetScissorRects(1, &scissor);
    frame.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    frame.list->SetGraphicsRootConstantBufferView(
        0, s->constants->GetGPUVirtualAddress() + static_cast<UINT64>(s->nextSlot) * s->constantStride);
    frame.list->DrawInstanced(3, 1, 0, 0);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    frame.list->ResourceBarrier(1, &barrier);
    hr = frame.list->Close();
    if (Failed(hr, "Close command list")) return false;

    ID3D12CommandList* lists[] = { frame.list.Get() };
    s->queue->ExecuteCommandLists(1, lists);
    hr = s->queue->Signal(s->sharedFence.Get(), publishValue);
    if (Failed(hr, "Signal shared fence")) return false;
    frame.completion = ++s->internalValue;
    hr = s->queue->Signal(s->internalFence.Get(), frame.completion);
    if (Failed(hr, "Signal internal fence")) return false;

    s->nextSlot = (s->nextSlot + 1) % kFrameCount;
    return true;
}

DPS_EXPORT uint64_t dps_get_completed_value(DPS_HANDLE handle) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    return s ? s->sharedFence->GetCompletedValue() : 0;
}
DPS_EXPORT void* dps_get_texture_handle(DPS_HANDLE handle) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    return s ? s->textureHandle : nullptr;
}
DPS_EXPORT void* dps_get_fence_handle(DPS_HANDLE handle) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    return s ? s->fenceHandle : nullptr;
}
DPS_EXPORT void* dps_get_device_ptr(DPS_HANDLE handle) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    return s ? s->device.Get() : nullptr;
}
DPS_EXPORT void* dps_get_resource_ptr(DPS_HANDLE handle) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    return s ? s->target.Get() : nullptr;
}
DPS_EXPORT int64_t dps_get_adapter_luid(DPS_HANDLE handle) {
    ShaderState* s = static_cast<ShaderState*>(handle);
    if (!s) return 0;
    const LUID luid = s->device->GetAdapterLuid();
    int64_t packed = 0;
    static_assert(sizeof(packed) == sizeof(luid));
    memcpy(&packed, &luid, sizeof(packed));
    return packed;
}
DPS_EXPORT const char* dps_last_error(void) { return g_error.c_str(); }
DPS_EXPORT void dps_close(DPS_HANDLE handle) { CloseState(static_cast<ShaderState*>(handle)); }

}
