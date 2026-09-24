#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPC_EXPORT __declspec(dllexport)
typedef void* DPC_HANDLE;

typedef struct DPC_WINDOW_STATE {
    int32_t alive;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t left_down;
    int32_t wheel_delta;
    int32_t key_code;
    uint64_t resize_serial;
} DPC_WINDOW_STATE;

// Creates one GPU console producer. Construction performs the only atlas upload
// and may wait for that upload; frame submission never waits on the CPU.
DPC_EXPORT DPC_HANDLE dpc_create(
    uint32_t width,
    uint32_t height,
    uint32_t max_cells,
    const wchar_t* texture_name,
    const wchar_t* fence_name,
    const wchar_t* atlas_png,
    const wchar_t* metrics_json);

// Packed cell ABI: bits 0..15 codepoint, 16..23 foreground, 24..31 background.
// Returns false when the next upload slot is still owned by the GPU. The caller
// should drop the stale frame and publish its newest state later.
DPC_EXPORT bool dpc_try_present(
    DPC_HANDLE handle,
    const uint32_t* cells,
    uint32_t cell_count,
    uint32_t columns,
    uint32_t rows,
    float time_seconds,
    uint64_t publish_value);

// Optional compatibility shim for the existing DirectPort example consumers.
// The render/fence object is fully usable without a manifest.
DPC_EXPORT bool dpc_enable_manifest(DPC_HANDLE handle, const wchar_t* manifest_name);

// Optional in-DLL viewer. It renders the same cell draw directly into a flip
// swapchain; no readback, external consumer, or WPF raster path is involved.
DPC_EXPORT bool dpc_show_window(DPC_HANDLE handle, const wchar_t* title, uint32_t width, uint32_t height);
DPC_EXPORT bool dpc_pump_window(DPC_HANDLE handle, uint32_t wait_ms, DPC_WINDOW_STATE* state);

DPC_EXPORT uint64_t dpc_get_completed_value(DPC_HANDLE handle);
DPC_EXPORT uint64_t dpc_get_dropped_frames(DPC_HANDLE handle);
DPC_EXPORT void* dpc_get_texture_handle(DPC_HANDLE handle);
DPC_EXPORT void* dpc_get_fence_handle(DPC_HANDLE handle);
DPC_EXPORT void* dpc_get_device_ptr(DPC_HANDLE handle);   // borrowed; do not Release
DPC_EXPORT void* dpc_get_resource_ptr(DPC_HANDLE handle); // borrowed; do not Release
DPC_EXPORT int64_t dpc_get_adapter_luid(DPC_HANDLE handle);
DPC_EXPORT const char* dpc_last_error(void);
DPC_EXPORT void dpc_close(DPC_HANDLE handle);

#ifdef __cplusplus
}
#endif
