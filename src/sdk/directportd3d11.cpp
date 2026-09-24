// --- directportd3d11.cpp ---
// DirectPort D3D11 Implementation
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <sddl.h>
#include <d3d12.h> 
#include "directport.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d12.lib")

extern "C" {

// Global Singleton States
static ID3D11Device5* g_dp11_device = NULL;
static ID3D11DeviceContext4* g_dp11_context = NULL;

// The NT Name Resolver. D3D11 physically lacks the API to resolve NT Handle Strings.
// We keep exactly ONE D3D12 device alive globally just to do string-to-handle lookups.
static ID3D12Device* g_dp11_resolver12 = NULL; 

typedef struct {
    ID3D11Texture2D* texture;
    ID3D11Fence* fence;
    HANDLE hSharedTex;
    HANDLE hSharedFence;
    HANDLE hEvent;
} DP11_State;

static DXGI_FORMAT GetDXGIFormat(DP_FORMAT fmt) {
    switch(fmt) {
        case DP_FORMAT_VIDEO:       return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DP_FORMAT_FLOAT:       return DXGI_FORMAT_R32_FLOAT;
        case DP_FORMAT_HALF:        return DXGI_FORMAT_R16_FLOAT;
        case DP_FORMAT_RAW_32BIT:   return DXGI_FORMAT_R32_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

// ----------------------------------------------------------------------------
// GLOBAL SUBSYSTEM LIFECYCLE
// ----------------------------------------------------------------------------
DP_EXPORT bool dp11_init(void) {
    if (g_dp11_device) return true; // Already initialized

    ID3D11Device* baseDevice = NULL;
    ID3D11DeviceContext* baseContext = NULL;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &baseDevice, NULL, &baseContext))) {
        return false;
    }
    
    baseDevice->QueryInterface(IID_PPV_ARGS(&g_dp11_device));
    baseContext->QueryInterface(IID_PPV_ARGS(&g_dp11_context));
    baseDevice->Release();
    baseContext->Release();

    // Initialize the D3D12 NT Handle resolution subsystem
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dp11_resolver12)))) {
        return false;
    }

    return (g_dp11_device != NULL && g_dp11_resolver12 != NULL);
}

DP_EXPORT void dp11_shutdown(void) {
    if (g_dp11_resolver12) { g_dp11_resolver12->Release(); g_dp11_resolver12 = NULL; }
    if (g_dp11_context)    { g_dp11_context->Release();    g_dp11_context = NULL; }
    if (g_dp11_device)     { g_dp11_device->Release();     g_dp11_device = NULL; }
}

// ----------------------------------------------------------------------------
// RESOURCE MANAGEMENT
// ----------------------------------------------------------------------------
DP_EXPORT DP_HANDLE dp11_create_shared_resource(uint32_t width, uint32_t height, DP_FORMAT format, const wchar_t* tex_name, const wchar_t* fence_name) {
    if (!g_dp11_device) return NULL; // Safety check: requires dp11_init()

    DP11_State* state = (DP11_State*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DP11_State));

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = GetDXGIFormat(format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;

    g_dp11_device->CreateTexture2D(&desc, NULL, &state->texture);

    PSECURITY_DESCRIPTOR sd = NULL;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };

    IDXGIResource1* res;
    state->texture->QueryInterface(IID_PPV_ARGS(&res));
    res->CreateSharedHandle(&sa, GENERIC_ALL, tex_name, &state->hSharedTex);
    res->Release();

    g_dp11_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&state->fence);
    state->fence->CreateSharedHandle(&sa, GENERIC_ALL, fence_name, &state->hSharedFence);

    LocalFree(sd);
    state->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    return state;
}

DP_EXPORT DP_HANDLE dp11_open_shared_resource(const wchar_t* tex_name, const wchar_t* fence_name) {
    if (!g_dp11_device || !g_dp11_resolver12) return NULL; // Safety check

    DP11_State* state = (DP11_State*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DP11_State));
    HANDLE hTexHandle = NULL, hFenceHandle = NULL;
    
    // Synchronously resolve NT Handles using the global D3D12 helper (No device creation overhead!)
    g_dp11_resolver12->OpenSharedHandleByName(tex_name, GENERIC_ALL, &hTexHandle);
    g_dp11_resolver12->OpenSharedHandleByName(fence_name, GENERIC_ALL, &hFenceHandle);

    if (hTexHandle == NULL || hFenceHandle == NULL) {
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    g_dp11_device->OpenSharedResource1(hTexHandle, __uuidof(ID3D11Texture2D), (void**)&state->texture);
    g_dp11_device->OpenSharedFence(hFenceHandle, __uuidof(ID3D11Fence), (void**)&state->fence);

    state->hSharedTex = hTexHandle;
    state->hSharedFence = hFenceHandle;
    state->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    return state;
}

DP_EXPORT void dp11_signal_fence(DP_HANDLE handle, uint64_t frame_value) {
    DP11_State* state = (DP11_State*)handle;
    g_dp11_context->Signal(state->fence, frame_value);
}

DP_EXPORT void dp11_wait_fence(DP_HANDLE handle, uint64_t target_value) {
    DP11_State* state = (DP11_State*)handle;
    if (state->fence->GetCompletedValue() < target_value) {
        g_dp11_context->Wait(state->fence, target_value); 
    }
}

DP_EXPORT void dp11_close(DP_HANDLE handle) {
    DP11_State* state = (DP11_State*)handle;
    if (state->hEvent) CloseHandle(state->hEvent);
    if (state->hSharedTex) CloseHandle(state->hSharedTex);
    if (state->hSharedFence) CloseHandle(state->hSharedFence);
    if (state->fence) state->fence->Release();
    if (state->texture) state->texture->Release();
    
    // Note: Device lifecycle is managed globally. Do not release the global device or context here.
    
    HeapFree(GetProcessHeap(), 0, state);
}

}