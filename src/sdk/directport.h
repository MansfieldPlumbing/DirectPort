// --- directport.h ---
// DirectPort Interprocess GPU Memory Protocol
// Version 6.1 - Defect corrections: NT handle leak fix, wait function split,
//               HRESULT propagation, null checks, SDDL validation.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DP_EXPORT __declspec(dllexport)

// Data Format Enum (Media Agnostic)
// Defines memory layout only. Semantic meaning is determined by the Producer/Consumer.
typedef enum {
    DP_FORMAT_VIDEO     = 0, // DXGI_FORMAT_B8G8R8A8_UNORM (4 bytes per unit)
    DP_FORMAT_FLOAT     = 1, // DXGI_FORMAT_R32_FLOAT      (4 bytes per unit)
    DP_FORMAT_HALF      = 2, // DXGI_FORMAT_R16_FLOAT      (2 bytes per unit)
    DP_FORMAT_RAW_32BIT = 3  // DXGI_FORMAT_R32_UINT       (4 bytes per unit)
} DP_FORMAT;

typedef void* DP_HANDLE;

// ============================================================================
// D3D12 API (Primary Implementation)
// ============================================================================

// Initializes the DirectPort D3D12 Subsystem (Global Singleton State).
// MUST be called once per process before creating/opening handles.
// ARCHITECTURAL CONSTRAINT (v1.0): Single global adapter. Multi-card deployments
// (e.g., two V340L cards, 4 dies) require refactor to pass ID3D12Device* explicitly.
DP_EXPORT bool      dp12_init(void);

// Tears down the global D3D12 subsystem on application exit.
DP_EXPORT void      dp12_shutdown(void);

// Creates a shared GPU resource with an NT Handle.
// Returns NULL on any allocation or handle creation failure.
DP_EXPORT DP_HANDLE dp12_create_shared_resource(uint32_t width, uint32_t height, DP_FORMAT format, bool is_system_ram, const wchar_t* tex_name, const wchar_t* fence_name);

// Opens an existing shared GPU resource by NT Handle Name.
// Returns NULL if either name cannot be resolved or handles cannot be opened.
DP_EXPORT DP_HANDLE dp12_open_shared_resource(const wchar_t* tex_name, const wchar_t* fence_name);

// Maps resource memory to CPU address. Returns row_pitch aligned to 256 bytes.
// ONLY valid if resource was created with is_system_ram = true.
DP_EXPORT void*     dp12_map_memory(DP_HANDLE handle, uint32_t* out_row_pitch);

// Unmaps resource memory.
DP_EXPORT void      dp12_unmap_memory(DP_HANDLE handle);

// Signals the DirectX Fence (GPU Async). Pushes frame availability.
DP_EXPORT void      dp12_signal_fence(DP_HANDLE handle, uint64_t frame_value);

// CPU blocks until fence reaches target_value.
// USE ONLY for final output token readback. Incurs OS scheduler latency (1ms+).
// Do NOT use in the inference loop — use dp12_queue_wait instead.
DP_EXPORT void      dp12_cpu_wait(DP_HANDLE handle, uint64_t target_value);

// GPU command queue waits on fence at hardware level. CPU returns immediately.
// Preserves 170ns PCIe crossbar fence latency. Zero OS scheduler involvement.
// USE for all internal GPU-to-GPU pipeline synchronization.
#ifdef __cplusplus
DP_EXPORT void      dp12_queue_wait(DP_HANDLE handle, struct ID3D12CommandQueue* pQueue, uint64_t target_value);
#endif

// Returns the current completed fence value.
DP_EXPORT uint64_t  dp12_get_completed_value(DP_HANDLE handle);

// Returns the underlying NT Handle for the resource (for external interop).
DP_EXPORT void*     dp12_get_resource_handle(DP_HANDLE handle);

// Returns the underlying NT Handle for the fence (for external interop).
DP_EXPORT void*     dp12_get_fence_handle(DP_HANDLE handle);

// Releases resources, closes NT Handles, and frees state for this connection.
// After this call, handle is invalid. NT Object Manager reference count is
// decremented — VRAM is freed when all handles across all processes are closed.
DP_EXPORT void      dp12_close(DP_HANDLE handle);


// ============================================================================
// D3D11 API (D3D11 Compatibility Layer)
// ============================================================================

// Initializes the DirectPort D3D11 Subsystem and the NT Name Resolver.
// MUST be called once per process before creating/opening D3D11 handles.
DP_EXPORT bool      dp11_init(void);

// Tears down the global D3D11 subsystem on application exit.
DP_EXPORT void      dp11_shutdown(void);

DP_EXPORT DP_HANDLE dp11_create_shared_resource(uint32_t width, uint32_t height, DP_FORMAT format, const wchar_t* tex_name, const wchar_t* fence_name);
DP_EXPORT DP_HANDLE dp11_open_shared_resource(const wchar_t* tex_name, const wchar_t* fence_name);

DP_EXPORT void      dp11_signal_fence(DP_HANDLE handle, uint64_t frame_value);
// dp11_wait_fence issues a GPU hardware queue wait (non-blocking CPU).
// Matches the 170ns hardware fence path. See dp12_queue_wait for D3D12 equivalent.
DP_EXPORT void      dp11_wait_fence(DP_HANDLE handle, uint64_t target_value);
DP_EXPORT void      dp11_close(DP_HANDLE handle);

#ifdef __cplusplus
}
#endif
