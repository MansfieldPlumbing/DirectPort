#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dxgiformat.h>
#include <sddl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "DirectPort.Console.Native.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr uint32_t kFrameCount = 3;
constexpr uint32_t kGlyphCount = 0x10000;
thread_local std::string g_error;

struct GlyphGpu {
    float atlas[4]; // left, bottom, right, top
    float plane[4]; // left, bottom, right, top
};

struct ConsoleConstants {
    float resolution[2];
    uint32_t grid[2];
    float cell_size[2];
    float time;
    float distance_range;
    float em_px_in_atlas;
    float line_height;
    float ascender;
    float atlas_width;
    float atlas_height;
    float fixed_advance;
    float padding;
};
static_assert(sizeof(ConsoleConstants) == 60); // HLSL pads the final register to 64 bytes.

struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> cells;
    ComPtr<ID3D12Resource> constants;
    uint8_t* mapped_cells = nullptr;
    uint8_t* mapped_constants = nullptr;
    uint64_t completion = 0;
};

struct BroadcastManifest {
    uint64_t frame_value;
    uint32_t width;
    uint32_t height;
    DXGI_FORMAT format;
    LUID adapter_luid;
    wchar_t texture_name[256];
    wchar_t fence_name[256];
};

struct ConsoleState {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t max_cells = 0;
    uint32_t next_slot = 0;
    uint64_t internal_value = 0;
    uint64_t dropped_frames = 0;
    float distance_range = 4.0f;
    float em_px_in_atlas = 33.0f;
    float line_height = 1.162109375f;
    float ascender = 0.927734375f;
    float fixed_advance = 0.5859375f;
    uint32_t atlas_width = 0;
    uint32_t atlas_height = 0;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> target;
    ComPtr<ID3D12Resource> atlas;
    ComPtr<ID3D12Resource> glyphs;
    ComPtr<ID3D12Fence> shared_fence;
    ComPtr<ID3D12Fence> internal_fence;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<IDXGISwapChain3> swap_chain;
    ComPtr<ID3D12DescriptorHeap> view_rtv_heap;
    ComPtr<ID3D12Resource> back_buffers[2];
    uint8_t* mapped_glyphs = nullptr;
    FrameSlot frames[kFrameCount];

    HANDLE texture_handle = nullptr;
    HANDLE fence_handle = nullptr;
    HANDLE event_handle = nullptr;
    HANDLE manifest_handle = nullptr;
    BroadcastManifest* manifest = nullptr;
    std::wstring texture_name;
    std::wstring fence_name;
    HWND hwnd = nullptr;
    uint32_t view_width = 0;
    uint32_t view_height = 0;
    uint32_t pending_view_width = 0;
    uint32_t pending_view_height = 0;
    uint32_t view_rtv_stride = 0;
    uint32_t swapchain_flags = 0;
    int32_t mouse_x = 0;
    int32_t mouse_y = 0;
    int32_t wheel_delta = 0;
    int32_t key_code = 0;
    bool left_down = false;
    bool close_requested = false;
    uint64_t resize_serial = 0;
    bool com_initialized = false;
};

constexpr wchar_t kWindowClass[] = L"DirectPort.Console.D3D12";

LRESULT CALLBACK ConsoleWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    ConsoleState* s = reinterpret_cast<ConsoleState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        s = static_cast<ConsoleState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                s->pending_view_width = std::max<uint32_t>(1, LOWORD(lparam));
                s->pending_view_height = std::max<uint32_t>(1, HIWORD(lparam));
                ++s->resize_serial;
            }
            return 0;
        case WM_MOUSEMOVE:
            s->mouse_x = GET_X_LPARAM(lparam);
            s->mouse_y = GET_Y_LPARAM(lparam);
            return 0;
        case WM_LBUTTONDOWN:
            s->left_down = true;
            s->mouse_x = GET_X_LPARAM(lparam);
            s->mouse_y = GET_Y_LPARAM(lparam);
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            s->left_down = false;
            s->mouse_x = GET_X_LPARAM(lparam);
            s->mouse_y = GET_Y_LPARAM(lparam);
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL:
            s->wheel_delta += GET_WHEEL_DELTA_WPARAM(wparam);
            return 0;
        case WM_KEYDOWN:
            s->key_code = static_cast<int32_t>(wparam);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            s->hwnd = nullptr;
            s->close_requested = true;
            return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool Failed(HRESULT hr, const char* operation) {
    if (SUCCEEDED(hr)) return false;
    char buffer[320];
    sprintf_s(buffer, "%s failed (HRESULT 0x%08X)", operation, static_cast<unsigned>(hr));
    g_error = buffer;
    return true;
}

void SetCompileError(const char* stage, ID3DBlob* errors, HRESULT hr) {
    char code[24];
    sprintf_s(code, " (HRESULT 0x%08X)", static_cast<unsigned>(hr));
    g_error = stage;
    g_error += code;
    if (errors && errors->GetBufferPointer()) {
        g_error += ": ";
        g_error.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    }
}

bool ReadFileUtf8(const wchar_t* path, std::string& output) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
        g_error = "could not open glyph metrics JSON";
        return false;
    }
    _fseeki64(f, 0, SEEK_END);
    const __int64 size = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    if (size <= 0 || size > 128ll * 1024ll * 1024ll) {
        fclose(f);
        g_error = "glyph metrics JSON has an invalid size";
        return false;
    }
    output.resize(static_cast<size_t>(size));
    const size_t got = fread(output.data(), 1, output.size(), f);
    fclose(f);
    if (got != output.size()) {
        g_error = "could not read glyph metrics JSON";
        return false;
    }
    return true;
}

bool NumberAfter(const std::string& text, size_t begin, size_t end, const char* key, float& value) {
    std::string needle = "\"";
    needle += key;
    needle += "\"";
    size_t p = text.find(needle, begin);
    if (p == std::string::npos || p >= end) return false;
    p = text.find(':', p + needle.size());
    if (p == std::string::npos || p >= end) return false;
    char* after = nullptr;
    value = strtof(text.c_str() + p + 1, &after);
    return after != text.c_str() + p + 1 && static_cast<size_t>(after - text.c_str()) <= end;
}

bool ParseMetrics(const wchar_t* path, std::vector<GlyphGpu>& glyphs, ConsoleState* s) {
    std::string json;
    if (!ReadFileUtf8(path, json)) return false;
    const size_t all = json.size();
    const size_t atlas = json.find("\"atlas\"");
    const size_t metrics = json.find("\"metrics\"");
    const size_t glyph_array = json.find("\"glyphs\"");
    if (atlas == std::string::npos || metrics == std::string::npos || glyph_array == std::string::npos) {
        g_error = "glyph metrics JSON is missing atlas, metrics, or glyphs";
        return false;
    }
    NumberAfter(json, atlas, metrics, "distanceRange", s->distance_range);
    NumberAfter(json, atlas, metrics, "size", s->em_px_in_atlas);
    NumberAfter(json, metrics, glyph_array, "lineHeight", s->line_height);
    NumberAfter(json, metrics, glyph_array, "ascender", s->ascender);

    glyphs.assign(kGlyphCount, {});
    size_t p = json.find("\"unicode\"", glyph_array);
    bool have_advance = false;
    while (p != std::string::npos) {
        const size_t next = json.find("\"unicode\"", p + 9);
        const size_t end = next == std::string::npos ? all : next;
        float unicode_value = -1.0f;
        if (!NumberAfter(json, p, end, "unicode", unicode_value)) break;
        const int cp = static_cast<int>(unicode_value);
        float advance = 0.0f;
        if (!have_advance && NumberAfter(json, p, end, "advance", advance) && advance > 0.0f) {
            s->fixed_advance = advance;
            have_advance = true;
        }
        const size_t plane = json.find("\"planeBounds\"", p);
        const size_t atlas_bounds = json.find("\"atlasBounds\"", p);
        if (cp >= 0 && cp < static_cast<int>(kGlyphCount) &&
            plane != std::string::npos && plane < end &&
            atlas_bounds != std::string::npos && atlas_bounds < end) {
            GlyphGpu& g = glyphs[cp];
            if (!NumberAfter(json, atlas_bounds, end, "left", g.atlas[0]) ||
                !NumberAfter(json, atlas_bounds, end, "bottom", g.atlas[1]) ||
                !NumberAfter(json, atlas_bounds, end, "right", g.atlas[2]) ||
                !NumberAfter(json, atlas_bounds, end, "top", g.atlas[3]) ||
                !NumberAfter(json, plane, atlas_bounds, "left", g.plane[0]) ||
                !NumberAfter(json, plane, atlas_bounds, "bottom", g.plane[1]) ||
                !NumberAfter(json, plane, atlas_bounds, "right", g.plane[2]) ||
                !NumberAfter(json, plane, atlas_bounds, "top", g.plane[3])) {
                g = {};
            }
        }
        p = next;
    }
    return true;
}

bool DecodePngWic(const wchar_t* path, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (Failed(hr, "Create WIC factory")) return false;
    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (Failed(hr, "Open atlas PNG")) return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (Failed(hr, "Read atlas frame")) return false;
    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (Failed(hr, "Read atlas dimensions") || !w || !h) return false;
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(&converter);
    if (Failed(hr, "Create atlas converter")) return false;
    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                               nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (Failed(hr, "Convert atlas to RGBA8")) return false;
    rgba.resize(static_cast<size_t>(w) * h * 4);
    hr = converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data());
    if (Failed(hr, "Decode atlas pixels")) return false;
    width = w;
    height = h;
    return true;
}

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* device, uint64_t bytes, const char* label) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<uint64_t>(bytes, 256);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&result));
    if (Failed(hr, label)) result.Reset();
    return result;
}

bool WaitForGpu(ConsoleState* s) {
    const uint64_t value = ++s->internal_value;
    HRESULT hr = s->queue->Signal(s->internal_fence.Get(), value);
    if (Failed(hr, "Signal setup fence")) return false;
    if (s->internal_fence->GetCompletedValue() < value) {
        hr = s->internal_fence->SetEventOnCompletion(value, s->event_handle);
        if (Failed(hr, "Set setup fence event")) return false;
        WaitForSingleObject(s->event_handle, INFINITE);
    }
    return true;
}

bool CreateViewTargets(ConsoleState* s) {
    if (!s->swap_chain || !s->view_rtv_heap) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->view_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (uint32_t i = 0; i < 2; ++i) {
        HRESULT hr = s->swap_chain->GetBuffer(i, IID_PPV_ARGS(&s->back_buffers[i]));
        if (Failed(hr, "Get swapchain buffer")) return false;
        s->device->CreateRenderTargetView(s->back_buffers[i].Get(), nullptr, rtv);
        rtv.ptr += s->view_rtv_stride;
    }
    return true;
}

bool ResizeViewer(ConsoleState* s) {
    if (!s->swap_chain || !s->pending_view_width || !s->pending_view_height) return true;
    if (s->view_width == s->pending_view_width && s->view_height == s->pending_view_height) return true;
    // Resize is opportunistic. If any submitted list still references a back
    // buffer, keep presenting the old (DXGI-stretched) buffers and try later.
    const uint64_t completed = s->internal_fence->GetCompletedValue();
    for (const auto& frame : s->frames)
        if (frame.completion > completed) return true;
    for (auto& buffer : s->back_buffers) buffer.Reset();
    const uint32_t width = s->pending_view_width;
    const uint32_t height = s->pending_view_height;
    HRESULT hr = s->swap_chain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, s->swapchain_flags);
    if (Failed(hr, "Resize console swapchain")) return false;
    s->view_width = width;
    s->view_height = height;
    return CreateViewTargets(s);
}

const char* kConsoleHlsl = R"HLSL(
cbuffer ConsoleConstants : register(b0) {
    float2 Resolution;
    uint2 Grid;
    float2 CellSize;
    float Time;
    float DistanceRange;
    float EmPxInAtlas;
    float LineHeight;
    float Ascender;
    float AtlasWidth;
    float AtlasHeight;
    float FixedAdvance;
    float Padding;
};

struct Glyph { float4 atlas; float4 plane; };
StructuredBuffer<uint> Cells : register(t0);
StructuredBuffer<Glyph> Glyphs : register(t1);
Texture2D<float4> Atlas : register(t2);
SamplerState AtlasSampler : register(s0);

struct PixelInput { float4 position : SV_POSITION; };

PixelInput VSMain(uint id : SV_VertexID) {
    PixelInput o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}

float3 Base16(uint i) {
    static const uint c[16] = {
        0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00,
        0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
        0x767676, 0xE74856, 0x16C60C, 0xF9F1A5,
        0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2
    };
    uint v = c[min(i, 15)];
    return float3((v >> 16) & 255, (v >> 8) & 255, v & 255) / 255.0;
}

float3 Xterm(uint i) {
    if (i < 16) return Base16(i);
    if (i < 232) {
        uint n = i - 16;
        uint r = n / 36, g = (n / 6) % 6, b = n % 6;
        float3 q = float3(r, g, b);
        return lerp(55.0 / 255.0, 1.0, max(q - 1.0, 0.0) / 4.0) * step(0.5, q)
             + (q * (95.0 / 255.0)) * (1.0 - step(0.5, q));
    }
    float gray = (8.0 + 10.0 * (i - 232)) / 255.0;
    return gray.xxx;
}

float Median3(float3 v) { return max(min(v.r, v.g), min(max(v.r, v.g), v.b)); }

float4 PSMain(PixelInput input) : SV_TARGET {
    float2 pixel = input.position.xy;
    uint2 cellXY = min((uint2)floor(pixel / CellSize), Grid - 1);
    uint packed = Cells[cellXY.y * Grid.x + cellXY.x];
    uint cp = packed & 0xFFFF;
    float3 fg = Xterm((packed >> 16) & 255);
    float3 bg = Xterm((packed >> 24) & 255);
    Glyph glyph = Glyphs[cp];
    if (glyph.atlas.z <= glyph.atlas.x || glyph.atlas.w <= glyph.atlas.y)
        return float4(bg, 1.0);

    float2 local = pixel - float2(cellXY) * CellSize;
    float emPx = min(CellSize.y / LineHeight, CellSize.x / FixedAdvance);
    float xPad = (CellSize.x - FixedAdvance * emPx) * 0.5;
    float baseline = Ascender * emPx;
    float left = xPad + glyph.plane.x * emPx;
    float right = xPad + glyph.plane.z * emPx;
    float top = baseline - glyph.plane.w * emPx;
    float bottom = baseline - glyph.plane.y * emPx;
    if (local.x < left || local.x > right || local.y < top || local.y > bottom)
        return float4(bg, 1.0);

    float2 f = saturate((local - float2(left, top)) / float2(right - left, bottom - top));
    float au = lerp(glyph.atlas.x, glyph.atlas.z, f.x);
    float av = lerp(AtlasHeight - glyph.atlas.w, AtlasHeight - glyph.atlas.y, f.y);
    float3 msdf = Atlas.SampleLevel(AtlasSampler, float2(au / AtlasWidth, av / AtlasHeight), 0).rgb;
    float screenRange = max(1.0, DistanceRange * emPx / EmPxInAtlas);
    float coverage = saturate((Median3(msdf) - 0.5) * screenRange + 0.5);
    return float4(lerp(bg, fg, coverage), 1.0);
}
)HLSL";

void CloseState(ConsoleState* s) {
    if (!s) return;
    if (s->queue && s->internal_fence && s->event_handle) WaitForGpu(s);
    if (s->hwnd) DestroyWindow(s->hwnd);
    for (auto& frame : s->frames) {
        if (frame.cells && frame.mapped_cells) frame.cells->Unmap(0, nullptr);
        if (frame.constants && frame.mapped_constants) frame.constants->Unmap(0, nullptr);
        frame.mapped_cells = nullptr;
        frame.mapped_constants = nullptr;
    }
    if (s->glyphs && s->mapped_glyphs) s->glyphs->Unmap(0, nullptr);
    if (s->texture_handle) CloseHandle(s->texture_handle);
    if (s->fence_handle) CloseHandle(s->fence_handle);
    if (s->event_handle) CloseHandle(s->event_handle);
    if (s->manifest) UnmapViewOfFile(s->manifest);
    if (s->manifest_handle) CloseHandle(s->manifest_handle);
    if (s->com_initialized) CoUninitialize();
    delete s;
}
}

extern "C" {

DPC_EXPORT DPC_HANDLE dpc_create(uint32_t width, uint32_t height, uint32_t max_cells,
    const wchar_t* texture_name, const wchar_t* fence_name,
    const wchar_t* atlas_png, const wchar_t* metrics_json) {
    g_error.clear();
    if (!width || !height || !max_cells || !texture_name || !fence_name || !atlas_png || !metrics_json) {
        g_error = "dpc_create received a null or zero required argument";
        return nullptr;
    }
    ConsoleState* s = new (std::nothrow) ConsoleState();
    if (!s) { g_error = "out of memory"; return nullptr; }
    s->width = width;
    s->height = height;
    s->max_cells = max_cells;
    s->texture_name = texture_name;
    s->fence_name = fence_name;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s->com_initialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        Failed(hr, "CoInitializeEx"); CloseState(s); return nullptr;
    }

    std::vector<GlyphGpu> glyph_data;
    if (!ParseMetrics(metrics_json, glyph_data, s)) { CloseState(s); return nullptr; }
    std::vector<uint8_t> atlas_pixels;
    if (!DecodePngWic(atlas_png, atlas_pixels, s->atlas_width, s->atlas_height)) { CloseState(s); return nullptr; }

    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s->device));
    if (Failed(hr, "D3D12CreateDevice")) { CloseState(s); return nullptr; }
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&s->queue));
    if (Failed(hr, "CreateCommandQueue")) { CloseState(s); return nullptr; }
    hr = s->device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&s->shared_fence));
    if (Failed(hr, "Create shared fence")) { CloseState(s); return nullptr; }
    hr = s->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->internal_fence));
    if (Failed(hr, "Create internal fence")) { CloseState(s); return nullptr; }
    s->event_handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s->event_handle) { g_error = "CreateEvent failed"; CloseState(s); return nullptr; }

    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC target_desc = {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = width;
    target_desc.Height = height;
    target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1;
    target_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hr = s->device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_SHARED, &target_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&s->target));
    if (Failed(hr, "Create shared console texture")) { CloseState(s); return nullptr; }

    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, nullptr)) {
        g_error = "ConvertStringSecurityDescriptorToSecurityDescriptor failed"; CloseState(s); return nullptr;
    }
    SECURITY_ATTRIBUTES security = { sizeof(security), sd, FALSE };
    hr = s->device->CreateSharedHandle(s->target.Get(), &security, GENERIC_ALL, texture_name, &s->texture_handle);
    if (SUCCEEDED(hr)) hr = s->device->CreateSharedHandle(s->shared_fence.Get(), &security, GENERIC_ALL, fence_name, &s->fence_handle);
    LocalFree(sd);
    if (Failed(hr, "CreateSharedHandle")) { CloseState(s); return nullptr; }

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc = {};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = 1;
    hr = s->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&s->rtv_heap));
    if (Failed(hr, "Create RTV heap")) { CloseState(s); return nullptr; }
    s->device->CreateRenderTargetView(s->target.Get(), nullptr, s->rtv_heap->GetCPUDescriptorHandleForHeapStart());

    D3D12_DESCRIPTOR_HEAP_DESC srv_desc = {};
    srv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_desc.NumDescriptors = 1;
    srv_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = s->device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&s->srv_heap));
    if (Failed(hr, "Create SRV heap")) { CloseState(s); return nullptr; }

    D3D12_RESOURCE_DESC atlas_desc = {};
    atlas_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    atlas_desc.Width = s->atlas_width;
    atlas_desc.Height = s->atlas_height;
    atlas_desc.DepthOrArraySize = 1;
    atlas_desc.MipLevels = 1;
    atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    atlas_desc.SampleDesc.Count = 1;
    atlas_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    hr = s->device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &atlas_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s->atlas));
    if (Failed(hr, "Create atlas texture")) { CloseState(s); return nullptr; }
    D3D12_SHADER_RESOURCE_VIEW_DESC atlas_srv = {};
    atlas_srv.Format = atlas_desc.Format;
    atlas_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    atlas_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    atlas_srv.Texture2D.MipLevels = 1;
    s->device->CreateShaderResourceView(s->atlas.Get(), &atlas_srv, s->srv_heap->GetCPUDescriptorHandleForHeapStart());

    s->glyphs = CreateUploadBuffer(s->device.Get(), sizeof(GlyphGpu) * kGlyphCount, "Create glyph buffer");
    if (!s->glyphs) { CloseState(s); return nullptr; }
    D3D12_RANGE no_read = { 0, 0 };
    hr = s->glyphs->Map(0, &no_read, reinterpret_cast<void**>(&s->mapped_glyphs));
    if (Failed(hr, "Map glyph buffer")) { CloseState(s); return nullptr; }
    memcpy(s->mapped_glyphs, glyph_data.data(), glyph_data.size() * sizeof(GlyphGpu));

    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature = { D3D_ROOT_SIGNATURE_VERSION_1_0 };
    s->device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feature, sizeof(feature));
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 2;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 1;
    params[3].DescriptorTable.pDescriptorRanges = &range;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = _countof(params);
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, error_blob;
    hr = D3D12SerializeRootSignature(&root_desc, feature.HighestVersion, &root_blob, &error_blob);
    if (FAILED(hr)) { SetCompileError("root signature", error_blob.Get(), hr); CloseState(s); return nullptr; }
    hr = s->device->CreateRootSignature(0, root_blob->GetBufferPointer(), root_blob->GetBufferSize(), IID_PPV_ARGS(&s->root_signature));
    if (Failed(hr, "CreateRootSignature")) { CloseState(s); return nullptr; }

    ComPtr<ID3DBlob> vs, ps;
    const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    hr = D3DCompile(kConsoleHlsl, strlen(kConsoleHlsl), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", flags, 0, &vs, &error_blob);
    if (FAILED(hr)) { SetCompileError("console vertex shader", error_blob.Get(), hr); CloseState(s); return nullptr; }
    error_blob.Reset();
    hr = D3DCompile(kConsoleHlsl, strlen(kConsoleHlsl), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", flags, 0, &ps, &error_blob);
    if (FAILED(hr)) { SetCompileError("console pixel shader", error_blob.Get(), hr); CloseState(s); return nullptr; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = s->root_signature.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = target_desc.Format;
    pso.SampleDesc.Count = 1;
    hr = s->device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&s->pipeline));
    if (Failed(hr, "Create console pipeline")) { CloseState(s); return nullptr; }

    for (uint32_t i = 0; i < kFrameCount; ++i) {
        FrameSlot& frame = s->frames[i];
        frame.cells = CreateUploadBuffer(s->device.Get(), static_cast<uint64_t>(max_cells) * 4, "Create cell upload buffer");
        // Two 256-byte CBV slots: shared output geometry and local viewer geometry.
        frame.constants = CreateUploadBuffer(s->device.Get(), 512, "Create console constant buffer");
        if (!frame.cells || !frame.constants) { CloseState(s); return nullptr; }
        hr = frame.cells->Map(0, &no_read, reinterpret_cast<void**>(&frame.mapped_cells));
        if (Failed(hr, "Map cell upload buffer")) { CloseState(s); return nullptr; }
        hr = frame.constants->Map(0, &no_read, reinterpret_cast<void**>(&frame.mapped_constants));
        if (Failed(hr, "Map console constant buffer")) { CloseState(s); return nullptr; }
        hr = s->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame.allocator));
        if (Failed(hr, "CreateCommandAllocator")) { CloseState(s); return nullptr; }
        hr = s->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frame.allocator.Get(), nullptr, IID_PPV_ARGS(&frame.list));
        if (Failed(hr, "CreateCommandList")) { CloseState(s); return nullptr; }
        frame.list->Close();
    }

    // Atlas upload is the one intentional construction-time wait.
    const uint32_t src_pitch = s->atlas_width * 4;
    const uint32_t upload_pitch = (src_pitch + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    ComPtr<ID3D12Resource> atlas_upload = CreateUploadBuffer(s->device.Get(), static_cast<uint64_t>(upload_pitch) * s->atlas_height, "Create atlas upload buffer");
    if (!atlas_upload) { CloseState(s); return nullptr; }
    uint8_t* mapped = nullptr;
    hr = atlas_upload->Map(0, &no_read, reinterpret_cast<void**>(&mapped));
    if (Failed(hr, "Map atlas upload buffer")) { CloseState(s); return nullptr; }
    for (uint32_t y = 0; y < s->atlas_height; ++y)
        memcpy(mapped + static_cast<size_t>(y) * upload_pitch, atlas_pixels.data() + static_cast<size_t>(y) * src_pitch, src_pitch);
    atlas_upload->Unmap(0, nullptr);
    FrameSlot& setup = s->frames[0];
    setup.allocator->Reset();
    setup.list->Reset(setup.allocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = s->atlas.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = atlas_upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = atlas_desc.Format;
    src.PlacedFootprint.Footprint.Width = s->atlas_width;
    src.PlacedFootprint.Footprint.Height = s->atlas_height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = upload_pitch;
    setup.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_BARRIER atlas_barrier = {};
    atlas_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    atlas_barrier.Transition.pResource = s->atlas.Get();
    atlas_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    atlas_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    atlas_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    setup.list->ResourceBarrier(1, &atlas_barrier);
    setup.list->Close();
    ID3D12CommandList* setup_lists[] = { setup.list.Get() };
    s->queue->ExecuteCommandLists(1, setup_lists);
    if (!WaitForGpu(s)) { CloseState(s); return nullptr; }
    return s;
}

DPC_EXPORT bool dpc_show_window(DPC_HANDLE handle, const wchar_t* title, uint32_t width, uint32_t height) {
    ConsoleState* s = static_cast<ConsoleState*>(handle);
    if (!s || !title || !width || !height) { g_error = "invalid console window request"; return false; }
    if (s->hwnd) { g_error = "console window is already open"; return false; }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = ConsoleWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        g_error = "RegisterClassEx for console window failed"; return false;
    }
    RECT outer = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRectEx(&outer, WS_OVERLAPPEDWINDOW, FALSE, 0);
    s->close_requested = false;
    s->hwnd = CreateWindowExW(0, kWindowClass, title, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, outer.right - outer.left, outer.bottom - outer.top,
        nullptr, nullptr, wc.hInstance, s);
    if (!s->hwnd) { g_error = "CreateWindowEx for console failed"; return false; }

    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (Failed(hr, "CreateDXGIFactory2")) { DestroyWindow(s->hwnd); return false; }
    BOOL tearing = FALSE;
    if (SUCCEEDED(factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))) && tearing)
        s->swapchain_flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = s->swapchain_flags;
    ComPtr<IDXGISwapChain1> swap1;
    hr = factory->CreateSwapChainForHwnd(s->queue.Get(), s->hwnd, &desc, nullptr, nullptr, &swap1);
    if (Failed(hr, "Create console swapchain")) { DestroyWindow(s->hwnd); return false; }
    factory->MakeWindowAssociation(s->hwnd, DXGI_MWA_NO_ALT_ENTER);
    hr = swap1.As(&s->swap_chain);
    if (Failed(hr, "Query IDXGISwapChain3")) { DestroyWindow(s->hwnd); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = 2;
    hr = s->device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&s->view_rtv_heap));
    if (Failed(hr, "Create console view RTV heap")) { DestroyWindow(s->hwnd); return false; }
    s->view_rtv_stride = s->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s->view_width = s->pending_view_width = width;
    s->view_height = s->pending_view_height = height;
    if (!CreateViewTargets(s)) { DestroyWindow(s->hwnd); return false; }
    ShowWindow(s->hwnd, SW_SHOW);
    UpdateWindow(s->hwnd);
    return true;
}

DPC_EXPORT bool dpc_pump_window(DPC_HANDLE handle, uint32_t wait_ms, DPC_WINDOW_STATE* state) {
    ConsoleState* s = static_cast<ConsoleState*>(handle);
    if (!s || !state) { g_error = "invalid console window state request"; return false; }
    if (s->hwnd && wait_ms) {
        MsgWaitForMultipleObjectsEx(0, nullptr, wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    MSG message = {};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state->alive = s->hwnd && !s->close_requested;
    state->width = static_cast<int32_t>(s->pending_view_width);
    state->height = static_cast<int32_t>(s->pending_view_height);
    state->mouse_x = s->mouse_x;
    state->mouse_y = s->mouse_y;
    state->left_down = s->left_down;
    state->wheel_delta = s->wheel_delta;
    state->key_code = s->key_code;
    state->resize_serial = s->resize_serial;
    s->wheel_delta = 0;
    s->key_code = 0;
    return state->alive != 0;
}

DPC_EXPORT bool dpc_try_present(DPC_HANDLE handle, const uint32_t* cells, uint32_t cell_count,
    uint32_t columns, uint32_t rows, float time_seconds, uint64_t publish_value) {
    g_error.clear();
    ConsoleState* s = static_cast<ConsoleState*>(handle);
    if (!s) { g_error = "null console producer"; return false; }
    if (!cells || !columns || !rows || cell_count != columns * rows || cell_count > s->max_cells) {
        g_error = "invalid console cell payload or dimensions";
        return false;
    }
    FrameSlot& frame = s->frames[s->next_slot];
    if (frame.completion && s->internal_fence->GetCompletedValue() < frame.completion) {
        ++s->dropped_frames;
        return false;
    }
    if (!ResizeViewer(s)) return false;
    memcpy(frame.mapped_cells, cells, static_cast<size_t>(cell_count) * sizeof(uint32_t));
    ConsoleConstants constants = {};
    constants.resolution[0] = static_cast<float>(s->width);
    constants.resolution[1] = static_cast<float>(s->height);
    constants.grid[0] = columns;
    constants.grid[1] = rows;
    constants.cell_size[0] = static_cast<float>(s->width) / columns;
    constants.cell_size[1] = static_cast<float>(s->height) / rows;
    constants.time = time_seconds;
    constants.distance_range = s->distance_range;
    constants.em_px_in_atlas = s->em_px_in_atlas;
    constants.line_height = s->line_height;
    constants.ascender = s->ascender;
    constants.atlas_width = static_cast<float>(s->atlas_width);
    constants.atlas_height = static_cast<float>(s->atlas_height);
    constants.fixed_advance = s->fixed_advance;
    memcpy(frame.mapped_constants, &constants, sizeof(constants));
    if (s->hwnd && s->swap_chain) {
        ConsoleConstants view_constants = constants;
        view_constants.resolution[0] = static_cast<float>(s->view_width);
        view_constants.resolution[1] = static_cast<float>(s->view_height);
        view_constants.cell_size[0] = static_cast<float>(s->view_width) / columns;
        view_constants.cell_size[1] = static_cast<float>(s->view_height) / rows;
        memcpy(frame.mapped_constants + 256, &view_constants, sizeof(view_constants));
    }

    HRESULT hr = frame.allocator->Reset();
    if (Failed(hr, "Reset console allocator")) return false;
    hr = frame.list->Reset(frame.allocator.Get(), s->pipeline.Get());
    if (Failed(hr, "Reset console command list")) return false;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s->target.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    frame.list->ResourceBarrier(1, &barrier);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = s->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_VIEWPORT viewport = { 0, 0, static_cast<float>(s->width), static_cast<float>(s->height), 0, 1 };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(s->width), static_cast<LONG>(s->height) };
    ID3D12DescriptorHeap* heaps[] = { s->srv_heap.Get() };
    frame.list->SetDescriptorHeaps(1, heaps);
    frame.list->SetGraphicsRootSignature(s->root_signature.Get());
    frame.list->RSSetViewports(1, &viewport);
    frame.list->RSSetScissorRects(1, &scissor);
    frame.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    frame.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    frame.list->SetGraphicsRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress());
    frame.list->SetGraphicsRootShaderResourceView(1, frame.cells->GetGPUVirtualAddress());
    frame.list->SetGraphicsRootShaderResourceView(2, s->glyphs->GetGPUVirtualAddress());
    frame.list->SetGraphicsRootDescriptorTable(3, s->srv_heap->GetGPUDescriptorHandleForHeapStart());
    frame.list->DrawInstanced(3, 1, 0, 0);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    frame.list->ResourceBarrier(1, &barrier);

    // The optional local view is a second draw of the same cell/atlas buffers.
    // It gets its own geometry constants, so resize never stretches cell math.
    if (s->hwnd && s->swap_chain) {
        const uint32_t back_index = s->swap_chain->GetCurrentBackBufferIndex();
        D3D12_RESOURCE_BARRIER view_barrier = {};
        view_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        view_barrier.Transition.pResource = s->back_buffers[back_index].Get();
        view_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        view_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        view_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        frame.list->ResourceBarrier(1, &view_barrier);
        D3D12_CPU_DESCRIPTOR_HANDLE view_rtv = s->view_rtv_heap->GetCPUDescriptorHandleForHeapStart();
        view_rtv.ptr += static_cast<SIZE_T>(back_index) * s->view_rtv_stride;
        const D3D12_VIEWPORT view_viewport = { 0, 0, static_cast<float>(s->view_width), static_cast<float>(s->view_height), 0, 1 };
        const D3D12_RECT view_scissor = { 0, 0, static_cast<LONG>(s->view_width), static_cast<LONG>(s->view_height) };
        frame.list->RSSetViewports(1, &view_viewport);
        frame.list->RSSetScissorRects(1, &view_scissor);
        frame.list->OMSetRenderTargets(1, &view_rtv, FALSE, nullptr);
        frame.list->SetGraphicsRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress() + 256);
        frame.list->DrawInstanced(3, 1, 0, 0);
        std::swap(view_barrier.Transition.StateBefore, view_barrier.Transition.StateAfter);
        frame.list->ResourceBarrier(1, &view_barrier);
    }
    hr = frame.list->Close();
    if (Failed(hr, "Close console command list")) return false;
    ID3D12CommandList* lists[] = { frame.list.Get() };
    s->queue->ExecuteCommandLists(1, lists);
    if (s->hwnd && s->swap_chain) {
        const UINT present_flags = (s->swapchain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) ? DXGI_PRESENT_ALLOW_TEARING : 0;
        hr = s->swap_chain->Present(0, present_flags);
        if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED && Failed(hr, "Present console window")) return false;
    }
    hr = s->queue->Signal(s->shared_fence.Get(), publish_value);
    if (Failed(hr, "Signal console shared fence")) return false;
    if (s->manifest) {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&s->manifest->frame_value), static_cast<LONG64>(publish_value));
        WakeByAddressAll(&s->manifest->frame_value);
    }
    frame.completion = ++s->internal_value;
    hr = s->queue->Signal(s->internal_fence.Get(), frame.completion);
    if (Failed(hr, "Signal console slot fence")) return false;
    s->next_slot = (s->next_slot + 1) % kFrameCount;
    return true;
}

DPC_EXPORT bool dpc_enable_manifest(DPC_HANDLE handle, const wchar_t* manifest_name) {
    ConsoleState* s = static_cast<ConsoleState*>(handle);
    if (!s || !manifest_name || !manifest_name[0]) { g_error = "invalid manifest request"; return false; }
    if (s->manifest) { g_error = "a manifest is already enabled"; return false; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, nullptr)) {
        g_error = "manifest security descriptor creation failed"; return false;
    }
    SECURITY_ATTRIBUTES security = { sizeof(security), sd, FALSE };
    s->manifest_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
        sizeof(BroadcastManifest), manifest_name);
    LocalFree(sd);
    if (!s->manifest_handle) { g_error = "CreateFileMapping for manifest failed"; return false; }
    s->manifest = static_cast<BroadcastManifest*>(MapViewOfFile(s->manifest_handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest)));
    if (!s->manifest) {
        CloseHandle(s->manifest_handle); s->manifest_handle = nullptr;
        g_error = "MapViewOfFile for manifest failed"; return false;
    }
    ZeroMemory(s->manifest, sizeof(BroadcastManifest));
    s->manifest->width = s->width;
    s->manifest->height = s->height;
    s->manifest->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    s->manifest->adapter_luid = s->device->GetAdapterLuid();
    wcscpy_s(s->manifest->texture_name, s->texture_name.c_str());
    wcscpy_s(s->manifest->fence_name, s->fence_name.c_str());
    return true;
}

DPC_EXPORT uint64_t dpc_get_completed_value(DPC_HANDLE h) { auto s = static_cast<ConsoleState*>(h); return s ? s->shared_fence->GetCompletedValue() : 0; }
DPC_EXPORT uint64_t dpc_get_dropped_frames(DPC_HANDLE h) { auto s = static_cast<ConsoleState*>(h); return s ? s->dropped_frames : 0; }
DPC_EXPORT void* dpc_get_texture_handle(DPC_HANDLE h) { auto s = static_cast<ConsoleState*>(h); return s ? s->texture_handle : nullptr; }
DPC_EXPORT void* dpc_get_fence_handle(DPC_HANDLE h) { auto s = static_cast<ConsoleState*>(h); return s ? s->fence_handle : nullptr; }
DPC_EXPORT void* dpc_get_device_ptr(DPC_HANDLE h) { auto s = static_cast<ConsoleState*>(h); return s ? s->device.Get() : nullptr; }
DPC_EXPORT void* dpc_get_resource_ptr(DPC_HANDLE h) { auto s = static_cast<ConsoleState*>(h); return s ? s->target.Get() : nullptr; }
DPC_EXPORT int64_t dpc_get_adapter_luid(DPC_HANDLE h) {
    auto s = static_cast<ConsoleState*>(h); if (!s) return 0;
    const LUID luid = s->device->GetAdapterLuid(); int64_t packed = 0; memcpy(&packed, &luid, sizeof(packed)); return packed;
}
DPC_EXPORT const char* dpc_last_error(void) { return g_error.c_str(); }
DPC_EXPORT void dpc_close(DPC_HANDLE h) { CloseState(static_cast<ConsoleState*>(h)); }
}
