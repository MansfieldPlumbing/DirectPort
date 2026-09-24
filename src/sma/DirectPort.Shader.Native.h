#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPS_EXPORT __declspec(dllexport)
typedef void* DPS_HANDLE;

DPS_EXPORT DPS_HANDLE dps_create(
    uint32_t width,
    uint32_t height,
    int32_t format,
    const wchar_t* texture_name,
    const wchar_t* fence_name,
    const char* hlsl,
    const char* vertex_entry,
    const char* pixel_entry,
    uint32_t constant_capacity);

// Records a fullscreen-triangle draw and queues it. Returns false rather than
// blocking when all frame slots are still in flight.
DPS_EXPORT bool dps_try_draw(DPS_HANDLE handle, const void* constants, uint32_t constant_bytes, uint64_t publish_value);

DPS_EXPORT uint64_t dps_get_completed_value(DPS_HANDLE handle);
DPS_EXPORT void* dps_get_texture_handle(DPS_HANDLE handle);
DPS_EXPORT void* dps_get_fence_handle(DPS_HANDLE handle);
DPS_EXPORT void* dps_get_device_ptr(DPS_HANDLE handle);   // borrowed; do not Release
DPS_EXPORT void* dps_get_resource_ptr(DPS_HANDLE handle); // borrowed; do not Release
DPS_EXPORT int64_t dps_get_adapter_luid(DPS_HANDLE handle);
DPS_EXPORT const char* dps_last_error(void);
DPS_EXPORT void dps_close(DPS_HANDLE handle);

#ifdef __cplusplus
}
#endif
