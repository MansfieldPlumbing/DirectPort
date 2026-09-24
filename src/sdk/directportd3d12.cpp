// --- directportd3d12.cpp ---
// DirectPort D3D12 Implementation
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <sddl.h>
#include "directport.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "advapi32.lib")

extern "C" {

// Global Singleton State
// ARCHITECTURAL NOTE (v1.0 constraint): g_dp12_device is a single global adapter.
// This restricts the process to one physical adapter. Future revisions targeting
// multi-card deployments (e.g., two V340L cards, 4 dies total) must refactor to
// pass ID3D12Device* explicitly at init or handle creation time.
static ID3D12Device* g_dp12_device = NULL;

typedef struct {
    ID3D12Resource* resource;
    ID3D12Fence* fence;
    HANDLE hSharedTex;
    HANDLE hSharedFence;
    HANDLE hEvent;
} DP12_State;

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
DP_EXPORT bool dp12_init(void) {
    if (g_dp12_device) return true; // Already initialized
    
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dp12_device)))) {
        return false;
    }
    return true;
}

DP_EXPORT void dp12_shutdown(void) {
    if (g_dp12_device) {
        g_dp12_device->Release();
        g_dp12_device = NULL;
    }
}

// ----------------------------------------------------------------------------
// RESOURCE MANAGEMENT
// ----------------------------------------------------------------------------
// is_system_ram:
// - TRUE  : Enables CPU Map (dp12_map_memory). Uses CUSTOM Heap + ROW_MAJOR.
// - FALSE : GPU Access Only (Faster). Uses DEFAULT Heap + VRAM.
DP_EXPORT DP_HANDLE dp12_create_shared_resource(uint32_t width, uint32_t height, DP_FORMAT format, bool is_system_ram, const wchar_t* tex_name, const wchar_t* fence_name) {
    if (!g_dp12_device) return NULL;

    // FIX 4: Null check on HeapAlloc before any dereference.
    DP12_State* state = (DP12_State*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DP12_State));
    if (!state) return NULL;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = GetDXGIFormat(format);
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES props = {};
    if (is_system_ram) {
        props.Type = D3D12_HEAP_TYPE_CUSTOM;
        props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        props.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    } else {
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    }

    // FIX 3: Propagate HRESULT. On failure, clean up partial state and return NULL.
    HRESULT hr = g_dp12_device->CreateCommittedResource(
        &props,
        D3D12_HEAP_FLAG_SHARED | (is_system_ram ? D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER : 0),
        &desc,
        D3D12_RESOURCE_STATE_COMMON,
        NULL,
        IID_PPV_ARGS(&state->resource));
    if (FAILED(hr)) {
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    // FIX 5: Validate SDDL conversion before use. A NULL sd silently alters the ACL.
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        state->resource->Release();
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };

    // FIX 3 (continued): Check fence and handle creation.
    hr = g_dp12_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&state->fence));
    if (FAILED(hr)) {
        LocalFree(sd);
        state->resource->Release();
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    hr = g_dp12_device->CreateSharedHandle(state->resource, &sa, GENERIC_ALL, tex_name, &state->hSharedTex);
    if (FAILED(hr)) {
        LocalFree(sd);
        state->fence->Release();
        state->resource->Release();
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    hr = g_dp12_device->CreateSharedHandle(state->fence, &sa, GENERIC_ALL, fence_name, &state->hSharedFence);
    if (FAILED(hr)) {
        LocalFree(sd);
        CloseHandle(state->hSharedTex);
        state->fence->Release();
        state->resource->Release();
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    LocalFree(sd);
    state->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    return state;
}

DP_EXPORT DP_HANDLE dp12_open_shared_resource(const wchar_t* tex_name, const wchar_t* fence_name) {
    if (!g_dp12_device) return NULL;

    // FIX 4: Null check on HeapAlloc.
    DP12_State* state = (DP12_State*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DP12_State));
    if (!state) return NULL;

    // FIX 3 + FIX 6 (new defect): Check OpenSharedHandleByName before passing result
    // to OpenSharedHandle. A failed name lookup returns a NULL/invalid handle which
    // will fault on the subsequent OpenSharedHandle call.
    HRESULT hr = g_dp12_device->OpenSharedHandleByName(tex_name, GENERIC_ALL, &state->hSharedTex);
    if (FAILED(hr) || state->hSharedTex == NULL) {
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    hr = g_dp12_device->OpenSharedHandle(state->hSharedTex, IID_PPV_ARGS(&state->resource));
    if (FAILED(hr)) {
        CloseHandle(state->hSharedTex);
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    hr = g_dp12_device->OpenSharedHandleByName(fence_name, GENERIC_ALL, &state->hSharedFence);
    if (FAILED(hr) || state->hSharedFence == NULL) {
        state->resource->Release();
        CloseHandle(state->hSharedTex);
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    hr = g_dp12_device->OpenSharedHandle(state->hSharedFence, IID_PPV_ARGS(&state->fence));
    if (FAILED(hr)) {
        CloseHandle(state->hSharedFence);
        state->resource->Release();
        CloseHandle(state->hSharedTex);
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }

    state->hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    return state;
}

// ----------------------------------------------------------------------------
// CPU MEMORY ACCESS
// ----------------------------------------------------------------------------
// Maps the shared resource into CPU address space.
// CRITICAL: This only succeeds if the resource was created with is_system_ram = true.
// - is_system_ram = true  : CUSTOM Heap + ROW_MAJOR layout. CPU Map is VALID.
// - is_system_ram = false : DEFAULT Heap + VRAM storage. CPU Map is INVALID/UNDEFINED.
//
// For GPU-to-GPU IPC (e.g., D3D11 consumer, Compute Shader), set is_system_ram = false
// and skip this function entirely. Access memory via ShaderResourceView instead.
//
// Returns row_pitch aligned to 256 bytes (D3D12 Texture Requirement).
// Your data packing (scanlines) must respect this pitch to avoid corruption.
DP_EXPORT void* dp12_map_memory(DP_HANDLE handle, uint32_t* out_row_pitch) {
    DP12_State* state = (DP12_State*)handle;
    void* ptr = NULL;
    state->resource->Map(0, NULL, &ptr);
    if (out_row_pitch) {
        D3D12_RESOURCE_DESC desc = state->resource->GetDesc();
        uint32_t bpp = (desc.Format == DXGI_FORMAT_R16_FLOAT) ? 2 : 4;
        *out_row_pitch = (uint32_t)((desc.Width * bpp + 255) & ~255);
    }
    return ptr;
}

DP_EXPORT void dp12_unmap_memory(DP_HANDLE handle) {
    ((DP12_State*)handle)->resource->Unmap(0, NULL);
}

DP_EXPORT void dp12_signal_fence(DP_HANDLE handle, uint64_t frame_value) {
    ((DP12_State*)handle)->fence->Signal(frame_value);
}

// FIX 2: Split wait into two distinct functions.
//
// dp12_cpu_wait  — CPU blocks until fence reaches target_value.
//                  Use ONLY for final output token readback where CPU access is required.
//                  Incurs OS scheduler latency (1ms–15.6ms quantum).
//
// dp12_queue_wait — GPU command queue waits on fence at hardware level.
//                   Zero CPU involvement. Preserves 170ns PCIe crossbar fence latency.
//                   Use for ALL internal pipeline GPU-to-GPU synchronization.
//
// The original dp12_wait_fence (CPU block via WaitForSingleObject) is retained
// as dp12_cpu_wait for explicit CPU readback scenarios only.

DP_EXPORT void dp12_cpu_wait(DP_HANDLE handle, uint64_t target_value) {
    DP12_State* state = (DP12_State*)handle;
    if (state->fence->GetCompletedValue() < target_value) {
        state->fence->SetEventOnCompletion(target_value, state->hEvent);
        WaitForSingleObject(state->hEvent, INFINITE);
    }
}

DP_EXPORT void dp12_queue_wait(DP_HANDLE handle, ID3D12CommandQueue* pQueue, uint64_t target_value) {
    DP12_State* state = (DP12_State*)handle;
    // GPU hardware waits on fence — CPU thread returns immediately.
    // The command queue will not begin executing subsequent commands until
    // the fence reaches target_value. No OS scheduler involvement.
    pQueue->Wait(state->fence, target_value);
}

DP_EXPORT uint64_t dp12_get_completed_value(DP_HANDLE handle) {
    return ((DP12_State*)handle)->fence->GetCompletedValue();
}

DP_EXPORT void* dp12_get_resource_handle(DP_HANDLE handle) {
    return ((DP12_State*)handle)->hSharedTex;
}

DP_EXPORT void* dp12_get_fence_handle(DP_HANDLE handle) {
    return ((DP12_State*)handle)->hSharedFence;
}

DP_EXPORT void dp12_close(DP_HANDLE handle) {
    DP12_State* state = (DP12_State*)handle;
    if (state->hEvent)       CloseHandle(state->hEvent);
    // FIX 1: Close NT handles. Without these CloseHandle calls, the NT Object Manager
    // reference count for the VRAM allocation never reaches zero. Each dp12_close call
    // leaks the full VRAM allocation. On an 8GB VF partition this compounds fatally.
    if (state->hSharedFence) CloseHandle(state->hSharedFence);
    if (state->hSharedTex)   CloseHandle(state->hSharedTex);
    if (state->fence)        state->fence->Release();
    if (state->resource)     state->resource->Release();

    // Note: Device lifecycle is managed globally. Do not release the global device here.

    HeapFree(GetProcessHeap(), 0, state);
}

} // extern "C"
