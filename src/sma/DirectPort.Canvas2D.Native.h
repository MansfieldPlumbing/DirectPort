#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DP2D_EXPORT __declspec(dllexport)
typedef void* DP2D_HANDLE;

typedef struct DP2D_WINDOW_STATE {
    int32_t alive;
    int32_t width;
    int32_t height;
    int32_t mouse_x;
    int32_t mouse_y;
    int32_t left_down;
    int32_t right_down;
    int32_t wheel_delta;
    int32_t key_code;
    wchar_t char_code;
    int32_t is_double_click;
    uint64_t resize_serial;
} DP2D_WINDOW_STATE;

typedef enum DP2D_CURSOR {
    DP2D_CURSOR_ARROW = 0,
    DP2D_CURSOR_HAND = 1,
    DP2D_CURSOR_IBEAM = 2,
    DP2D_CURSOR_SIZEWE = 3,
    DP2D_CURSOR_SIZENS = 4,
    DP2D_CURSOR_SIZEALL = 5
} DP2D_CURSOR;

DP2D_EXPORT DP2D_HANDLE dp2d_create(
    uint32_t width,
    uint32_t height,
    const wchar_t* title);

DP2D_EXPORT bool dp2d_pump_window(DP2D_HANDLE handle, uint32_t wait_ms, DP2D_WINDOW_STATE* state);
DP2D_EXPORT void dp2d_set_cursor(DP2D_HANDLE handle, DP2D_CURSOR cursor);

DP2D_EXPORT bool dp2d_begin_frame(DP2D_HANDLE handle, uint32_t clear_color_argb);
DP2D_EXPORT void dp2d_fill_rect(DP2D_HANDLE handle, float x, float y, float w, float h, uint32_t argb, float radius);
DP2D_EXPORT void dp2d_draw_rect(DP2D_HANDLE handle, float x, float y, float w, float h, uint32_t argb, float stroke_width, float radius);
DP2D_EXPORT void dp2d_draw_line(DP2D_HANDLE handle, float x1, float y1, float x2, float y2, uint32_t argb, float stroke_width);
DP2D_EXPORT void dp2d_draw_text(DP2D_HANDLE handle, const wchar_t* text, float x, float y, float max_width, float max_height, uint32_t argb, float font_size, const wchar_t* font_family, bool bold, int32_t alignment);
DP2D_EXPORT bool dp2d_measure_text(DP2D_HANDLE handle, const wchar_t* text, float font_size, const wchar_t* font_family, bool bold, float* out_width, float* out_height);
DP2D_EXPORT void dp2d_push_clip(DP2D_HANDLE handle, float x, float y, float w, float h);
DP2D_EXPORT void dp2d_pop_clip(DP2D_HANDLE handle);
DP2D_EXPORT void dp2d_push_transform(DP2D_HANDLE handle, float offset_x, float offset_y, float scale);
DP2D_EXPORT void dp2d_pop_transform(DP2D_HANDLE handle);
DP2D_EXPORT void dp2d_set_opacity(DP2D_HANDLE handle, float alpha);
DP2D_EXPORT void dp2d_draw_shadow(DP2D_HANDLE handle, float x, float y, float w, float h, float radius, float blur, float offset_y, uint32_t argb);
DP2D_EXPORT void dp2d_draw_wire_icon(DP2D_HANDLE handle, const wchar_t* icon_name, float x, float y, float size, uint32_t argb, float stroke_width);

DP2D_EXPORT bool dp2d_end_frame(DP2D_HANDLE handle);
DP2D_EXPORT bool dp2d_save_png(DP2D_HANDLE handle, const wchar_t* file_path);
DP2D_EXPORT const char* dp2d_last_error(void);
DP2D_EXPORT void dp2d_close(DP2D_HANDLE handle);

#ifdef __cplusplus
}
#endif
