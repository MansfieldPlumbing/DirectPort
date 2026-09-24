// --- DirectPort.Canvas2D.Native.cpp ---
// DirectPort Native D3D12 Resource & Tile Compositor
// DirectWrite only bakes small glyph/icon visual tiles into the persistent Atlas;
// DirectWrite NEVER touches the swapchain backbuffer.
// The D3D12 compositor derives the frame from the population of instanced resource tiles & quads.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <d2d1.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <algorithm>
#include "DirectPort.Canvas2D.Native.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

static std::string g_LastError;

// ============================================================================
// HLSL SHADERS
// ============================================================================

const char* g_ShaderSource = R"(
cbuffer FrameConstants : register(b0) {
    float2 ViewportSize;
    float2 Pad0;
};

struct InstanceData {
    float2 Pos       : POSITION;
    float2 Size      : SIZE;
    float4 UV        : TEXCOORD0; // u0, v0, u1, v1
    uint   Color     : COLOR;     // ARGB
    float  Radius    : RADIUS;
    float  StrokeW   : STROKEW;
    uint   Mode      : MODE;      // 0: Rect/SDF, 1: Shadow, 2: GlyphTile, 3: Line
    float4 Transform : TRANSFORM; // ox, oy, scale, pad
};

struct VS_OUTPUT {
    float4 PosH                       : SV_POSITION;
    float2 LocalPos                   : TEXCOORD0;
    nointerpolation float2 Size       : TEXCOORD1;
    float2 UV                         : TEXCOORD2;
    float4 Color                      : COLOR0;
    nointerpolation float  Radius     : RADIUS;
    nointerpolation float  StrokeW    : STROKEW;
    nointerpolation uint   Mode       : MODE;
};

Texture2D    g_Atlas   : register(t0);
SamplerState g_Sampler : register(s0);

float4 UnpackARGB(uint c) {
    float a = ((c >> 24) & 0xFF) / 255.0f;
    float r = ((c >> 16) & 0xFF) / 255.0f;
    float g = ((c >> 8)  & 0xFF) / 255.0f;
    float b = (c & 0xFF) / 255.0f;
    return float4(r, g, b, a);
}

VS_OUTPUT VSMain(InstanceData inst, uint vid : SV_VertexID) {
    VS_OUTPUT output;
    
    // Quad unit coords (0..1)
    float2 quadUV = float2(vid & 1, (vid >> 1) & 1);
    float2 localPos = quadUV * inst.Size;
    
    // Apply 2D transform (scale + offset)
    float2 worldPos = (inst.Pos + localPos) * inst.Transform.z + inst.Transform.xy;
    
    // Convert screen coordinates to NDC [-1, 1]
    float2 ndc = (worldPos / ViewportSize) * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    output.PosH = float4(ndc, 0.0f, 1.0f);
    
    output.LocalPos = localPos;
    output.Size = inst.Size;
    output.UV = lerp(inst.UV.xy, inst.UV.zw, quadUV);
    output.Color = UnpackARGB(inst.Color);
    output.Radius = inst.Radius;
    output.StrokeW = inst.StrokeW;
    output.Mode = inst.Mode;
    
    return output;
}

// Signed distance to rounded box
float RoundedBoxSDF(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + float2(r, r);
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - r;
}

float4 PSMain(VS_OUTPUT input) : SV_Target {
    float4 col = input.Color;
    
    if (input.Mode == 0) {
        // Solid / Rounded Rect & Stroke
        if (input.StrokeW > 0.0f) {
            float2 halfSize = input.Size * 0.5f;
            float2 centerPos = input.LocalPos - halfSize;
            float r = min(input.Radius, min(halfSize.x, halfSize.y));
            float d = RoundedBoxSDF(centerPos, halfSize, r);
            float strokeHalf = input.StrokeW * 0.5f;
            float strokeDist = abs(d + strokeHalf) - strokeHalf;
            float alpha = 1.0f - smoothstep(-0.75f, 0.75f, strokeDist);
            col.a *= alpha;
        } else if (input.Radius > 0.0f) {
            float2 halfSize = input.Size * 0.5f;
            float2 centerPos = input.LocalPos - halfSize;
            float r = min(input.Radius, min(halfSize.x, halfSize.y));
            float d = RoundedBoxSDF(centerPos, halfSize, r);
            float alpha = 1.0f - smoothstep(-0.75f, 0.75f, d);
            col.a *= alpha;
        }
    } else if (input.Mode == 1) {
        // Drop Shadow
        float blur = max(input.StrokeW, 1.0f);
        float pad = blur * 2.0f;
        float2 halfSize = input.Size * 0.5f;
        float2 centerPos = input.LocalPos - halfSize;
        float2 casterHalfSize = max(float2(0.0f, 0.0f), halfSize - float2(pad, pad));
        float r = min(input.Radius, min(casterHalfSize.x, casterHalfSize.y));
        float d = RoundedBoxSDF(centerPos, casterHalfSize, r);
        float shadowAlpha = (d <= 0.0f) ? 1.0f : exp(-(d * d) / (2.0f * blur * blur));
        col.a *= shadowAlpha;
    } else if (input.Mode == 2) {
        // Glyph / Raster Tile from Atlas
        float4 atlasSample = g_Atlas.Sample(g_Sampler, input.UV);
        float coverage = max(atlasSample.a, max(atlasSample.r, max(atlasSample.g, atlasSample.b)));
        col.a *= coverage;
    } else if (input.Mode == 3) {
        // Anti-aliased line
        float distToCenter = abs(input.LocalPos.y - (input.Size.y * 0.5f));
        float alpha = 1.0f - smoothstep(input.Size.y * 0.5f - 0.75f, input.Size.y * 0.5f + 0.75f, distToCenter);
        col.a *= alpha;
    }
    
    if (col.a <= 0.001f) {
        discard;
    }
    
    return col;
}
)";

// ============================================================================
// COMPOSITOR DATA STRUCTURES
// ============================================================================

struct GpuInstance {
    float posX, posY;
    float sizeX, sizeY;
    float u0, v0, u1, v1;
    uint32_t color;
    float radius;
    float strokeW;
    uint32_t mode;
    float transOx, transOy, transScale, transPad;
};

struct TileResource {
    uint32_t id;
    float u0, v0, u1, v1;
    float width;
    float height;
    float bearingX;
    float bearingY;
    float advanceX;
};

struct ScissorClip {
    float x, y, w, h;
};

struct TransformFrame {
    float ox, oy, scale;
};

// ============================================================================
// DYNAMIC TILE ATLAS (Persistent Visual Resource Cache)
// ============================================================================

class DynamicTileAtlas {
public:
    static constexpr uint32_t ATLAS_SIZE = 2048;
    
    ID3D12Device*             m_pDevice = nullptr;
    ID3D12Resource*           m_pAtlasTexture = nullptr;
    ID3D12Resource*           m_pUploadBuffer = nullptr;
    uint8_t*                  m_pUploadMemory = nullptr;
    
    IDWriteFactory*           m_pDWriteFactory = nullptr;
    ID2D1Factory*             m_pD2DFactory = nullptr;
    
    uint32_t                  m_PackCursorX = 2;
    uint32_t                  m_PackCursorY = 2;
    uint32_t                  m_PackRowHeight = 0;
    
    std::unordered_map<std::wstring, TileResource> m_TileCache;
    std::vector<D3D12_BOX>     m_PendingUploadBoxes;
    bool                      m_Dirty = false;
    uint32_t                  m_NextId = 1;
    std::mutex                m_Mutex;

    bool Initialize(ID3D12Device* pDevice) {
        m_pDevice = pDevice;
        
        // 1. Create D3D12 Atlas Texture (2048x2048 B8G8R8A8)
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = ATLAS_SIZE;
        desc.Height = ATLAS_SIZE;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
        
        HRESULT hr = pDevice->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr, IID_PPV_ARGS(&m_pAtlasTexture));
        if (FAILED(hr)) return false;

        // 2. Create Upload Staging Buffer
        uint64_t uploadSize = ATLAS_SIZE * ATLAS_SIZE * 4;
        D3D12_RESOURCE_DESC upDesc = {};
        upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upDesc.Width = uploadSize;
        upDesc.Height = 1;
        upDesc.DepthOrArraySize = 1;
        upDesc.MipLevels = 1;
        upDesc.Format = DXGI_FORMAT_UNKNOWN;
        upDesc.SampleDesc.Count = 1;
        upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        D3D12_HEAP_PROPERTIES upHeapProps = {};
        upHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        hr = pDevice->CreateCommittedResource(
            &upHeapProps, D3D12_HEAP_FLAG_NONE, &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&m_pUploadBuffer));
        if (FAILED(hr)) return false;

        m_pUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pUploadMemory));
        memset(m_pUploadMemory, 0, uploadSize);

        // 3. Create Offscreen DWrite/D2D Staging Engine for baking glyph visual tiles
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&m_pDWriteFactory));
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);

        return true;
    }

    TileResource GetOrBakeGlyph(wchar_t ch, float fontSize, const wchar_t* fontFamily, bool bold) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        std::wstring key = std::wstring(1, ch) + L"_" + std::to_wstring((int)fontSize) + L"_" + (fontFamily ? fontFamily : L"Segoe UI") + L"_" + (bold ? L"b" : L"n");
        auto it = m_TileCache.find(key);
        if (it != m_TileCache.end()) {
            return it->second;
        }

        // Measure & rasterize using standard Win32 GDI Font rasterization on 32-bit DIB
        HDC hdc = CreateCompatibleDC(nullptr);
        HFONT hFont = CreateFontW(
            -(int)roundf(fontSize * 1.333f), 0, 0, 0,
            bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            fontFamily ? fontFamily : L"Segoe UI");

        HGDIOBJ hOldFont = SelectObject(hdc, hFont);

        wchar_t str[2] = { ch, 0 };
        SIZE textSize = {};
        GetTextExtentPoint32W(hdc, str, 1, &textSize);

        uint32_t cellW = (uint32_t)std::max(4L, textSize.cx + 4L);
        uint32_t cellH = (uint32_t)std::max(4L, textSize.cy + 4L);

        // Allocate slot in atlas
        if (m_PackCursorX + cellW + 2 >= ATLAS_SIZE) {
            m_PackCursorX = 2;
            m_PackCursorY += m_PackRowHeight + 2;
            m_PackRowHeight = 0;
        }
        if (m_PackCursorY + cellH + 2 >= ATLAS_SIZE) {
            m_PackCursorX = 2;
            m_PackCursorY = 2;
            m_PackRowHeight = 0;
        }

        uint32_t slotX = m_PackCursorX;
        uint32_t slotY = m_PackCursorY;
        m_PackCursorX += cellW + 2;
        if (cellH > m_PackRowHeight) m_PackRowHeight = cellH;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = cellW;
        bmi.bmiHeader.biHeight = -(LONG)cellH; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HBITMAP hBmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        HGDIOBJ hOldBmp = SelectObject(hdc, hBmp);

        // Clear DIB section to black
        memset(pBits, 0, cellW * cellH * 4);

        SetBkMode(hdc, OPAQUE);
        SetBkColor(hdc, RGB(0, 0, 0));
        SetTextColor(hdc, RGB(255, 255, 255));

        RECT r = { 0, 0, (LONG)cellW, (LONG)cellH };
        DrawTextW(hdc, str, 1, &r, DT_LEFT | DT_TOP | DT_NOCLIP | DT_NOPREFIX);

        GdiFlush();

        // Copy baked DIB section directly into Upload Buffer with computed alpha coverage
        for (uint32_t y = 0; y < cellH; ++y) {
            uint32_t* pDst = reinterpret_cast<uint32_t*>(m_pUploadMemory + (slotY + y) * (ATLAS_SIZE * 4) + slotX * 4);
            const uint32_t* pSrc = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pBits) + y * (cellW * 4));
            for (uint32_t x = 0; x < cellW; ++x) {
                uint32_t pixel = pSrc[x];
                uint8_t r = ((pixel >> 16) & 0xFF);
                uint8_t g = ((pixel >> 8) & 0xFF);
                uint8_t b = (pixel & 0xFF);
                uint8_t a = std::max(r, std::max(g, b));
                pDst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }

        SelectObject(hdc, hOldBmp);
        DeleteObject(hBmp);
        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        DeleteDC(hdc);

        D3D12_BOX box = {};
        box.left = slotX;
        box.top = slotY;
        box.right = slotX + cellW;
        box.bottom = slotY + cellH;
        box.front = 0;
        box.back = 1;
        m_PendingUploadBoxes.push_back(box);
        m_Dirty = true;

        TileResource tile = {};
        tile.id = m_NextId++;
        tile.u0 = (float)slotX / (float)ATLAS_SIZE;
        tile.v0 = (float)slotY / (float)ATLAS_SIZE;
        tile.u1 = (float)(slotX + cellW) / (float)ATLAS_SIZE;
        tile.v1 = (float)(slotY + cellH) / (float)ATLAS_SIZE;
        tile.width = (float)cellW;
        tile.height = (float)cellH;
        tile.bearingX = 0.0f;
        tile.bearingY = 0.0f;
        tile.advanceX = (float)std::max(1L, textSize.cx);

        m_TileCache[key] = tile;
        return tile;
    }

    bool MeasureText(const wchar_t* text, float fontSize, const wchar_t* fontFamily, bool bold, float* outW, float* outH) {
        if (!text || !text[0]) {
            *outW = 0.0f; *outH = fontSize * 1.2f;
            return true;
        }

        HDC hdc = CreateCompatibleDC(nullptr);
        HFONT hFont = CreateFontW(
            -(int)roundf(fontSize * 1.333f), 0, 0, 0,
            bold ? FW_BOLD : FW_NORMAL,
            FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            fontFamily ? fontFamily : L"Segoe UI");

        HGDIOBJ hOldFont = SelectObject(hdc, hFont);

        SIZE textSize = {};
        GetTextExtentPoint32W(hdc, text, (int)wcslen(text), &textSize);

        *outW = (float)textSize.cx;
        *outH = (float)textSize.cy;

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        DeleteDC(hdc);

        return true;
    }

    void FlushUploads(ID3D12GraphicsCommandList* pCmdList) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Dirty || m_PendingUploadBoxes.empty() || !m_pAtlasTexture || !m_pUploadBuffer) return;

        // Transition Atlas to COPY_DEST
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_pAtlasTexture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        pCmdList->ResourceBarrier(1, &barrier);

        for (const auto& box : m_PendingUploadBoxes) {
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = m_pAtlasTexture;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = m_pUploadBuffer;
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = 0;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = ATLAS_SIZE;
            src.PlacedFootprint.Footprint.Height = ATLAS_SIZE;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = ATLAS_SIZE * 4;

            pCmdList->CopyTextureRegion(&dst, box.left, box.top, 0, &src, &box);
        }

        // Transition Atlas back to PIXEL_SHADER_RESOURCE
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        pCmdList->ResourceBarrier(1, &barrier);

        m_PendingUploadBoxes.clear();
        m_Dirty = false;
    }

    void Shutdown() {
        if (m_pUploadBuffer) {
            m_pUploadBuffer->Unmap(0, nullptr);
            m_pUploadBuffer->Release();
            m_pUploadBuffer = nullptr;
        }
        if (m_pAtlasTexture) { m_pAtlasTexture->Release(); m_pAtlasTexture = nullptr; }
        if (m_pDWriteFactory) { m_pDWriteFactory->Release(); m_pDWriteFactory = nullptr; }
        if (m_pD2DFactory) { m_pD2DFactory->Release(); m_pD2DFactory = nullptr; }
    }
};

// ============================================================================
// D3D12 2D RESOURCE COMPOSITOR
// ============================================================================

class D3D12Canvas {
public:
    static constexpr uint32_t MAX_INSTANCES_PER_FRAME = 65536;
    
    HWND                      m_hWnd = nullptr;
    uint32_t                  m_Width = 1280;
    uint32_t                  m_Height = 800;
    
    // Core D3D12 Objects
    ID3D12Device*             m_pDevice = nullptr;
    ID3D12CommandQueue*       m_pCommandQueue = nullptr;
    IDXGISwapChain3*          m_pSwapChain = nullptr;
    ID3D12DescriptorHeap*     m_pRtvHeap = nullptr;
    ID3D12DescriptorHeap*     m_pSrvHeap = nullptr;
    ID3D12Resource*           m_pRenderTargets[2] = { nullptr, nullptr };
    ID3D12CommandAllocator*   m_pCmdAllocators[2] = { nullptr, nullptr };
    ID3D12GraphicsCommandList* m_pCmdList = nullptr;
    ID3D12Fence*              m_pFence = nullptr;
    HANDLE                    m_hFenceEvent = nullptr;
    uint64_t                  m_FenceValues[2] = { 0, 0 };
    uint32_t                  m_FrameIndex = 0;
    
    // Pipeline & Root Signature
    ID3D12RootSignature*      m_pRootSignature = nullptr;
    ID3D12PipelineState*      m_pPSO = nullptr;
    ID3D12Resource*           m_pInstanceBuffer = nullptr;
    GpuInstance*              m_pMappedInstances = nullptr;
    uint32_t                  m_InstanceCount = 0;
    uint32_t                  m_InstanceCursor = 0;
    uint32_t                  m_BatchStart = 0;
    
    // Resource Atlas
    DynamicTileAtlas          m_Atlas;
    
    // State Stacks
    std::vector<ScissorClip>    m_ClipStack;
    std::vector<TransformFrame> m_TransformStack;
    float                     m_CurrentOpacity = 1.0f;
    
    // Window interaction
    DP2D_WINDOW_STATE         m_WindowState = {};
    HCURSOR                   m_Cursors[6] = {};

    bool Initialize(uint32_t width, uint32_t height, const wchar_t* title) {
        m_Width = width;
        m_Height = height;

        // Register window class
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = WndProcStatic;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"DirectPortCanvas2D_Class";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);

        m_Cursors[DP2D_CURSOR_ARROW]   = LoadCursor(nullptr, IDC_ARROW);
        m_Cursors[DP2D_CURSOR_HAND]    = LoadCursor(nullptr, IDC_HAND);
        m_Cursors[DP2D_CURSOR_IBEAM]   = LoadCursor(nullptr, IDC_IBEAM);
        m_Cursors[DP2D_CURSOR_SIZEWE]  = LoadCursor(nullptr, IDC_SIZEWE);
        m_Cursors[DP2D_CURSOR_SIZENS]  = LoadCursor(nullptr, IDC_SIZENS);
        m_Cursors[DP2D_CURSOR_SIZEALL] = LoadCursor(nullptr, IDC_SIZEALL);

        RECT r = { 0, 0, (LONG)width, (LONG)height };
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

        m_hWnd = CreateWindowExW(
            WS_EX_APPWINDOW,
            L"DirectPortCanvas2D_Class",
            title ? title : L"DirectPort Desktop",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT, CW_USEDEFAULT,
            r.right - r.left, r.bottom - r.top,
            nullptr, nullptr, GetModuleHandle(nullptr), this);

        if (!m_hWnd) {
            g_LastError = "CreateWindowExW failed";
            return false;
        }

        // 1. Create D3D12 Device
        IDXGIFactory6* pFactory = nullptr;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&pFactory));
        if (!pFactory) return false;

        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice));
        if (FAILED(hr)) return false;

        // 2. Create Command Queue
        D3D12_COMMAND_QUEUE_DESC qDesc = {};
        qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = m_pDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&m_pCommandQueue));
        if (FAILED(hr)) return false;

        // 3. Create Swap Chain
        DXGI_SWAP_CHAIN_DESC1 scDesc = {};
        scDesc.Width = width;
        scDesc.Height = height;
        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        IDXGISwapChain1* pSC1 = nullptr;
        pFactory->CreateSwapChainForHwnd(m_pCommandQueue, m_hWnd, &scDesc, nullptr, nullptr, &pSC1);
        pSC1->QueryInterface(IID_PPV_ARGS(&m_pSwapChain));
        pSC1->Release();
        pFactory->Release();

        m_FrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

        // 4. Create RTV Descriptor Heap
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 2;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        m_pDevice->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_pRtvHeap));
        uint32_t rtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < 2; i++) {
            m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_pRenderTargets[i]));
            m_pDevice->CreateRenderTargetView(m_pRenderTargets[i], nullptr, rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
            m_pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_pCmdAllocators[i]));
        }

        // 5. Create SRV Descriptor Heap (Atlas Texture)
        D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
        srvHeapDesc.NumDescriptors = 1;
        srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        m_pDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&m_pSrvHeap));

        // 6. Initialize Tile Atlas
        if (!m_Atlas.Initialize(m_pDevice)) {
            g_LastError = "Tile Atlas init failed";
            return false;
        }

        // Create SRV for Atlas
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        m_pDevice->CreateShaderResourceView(m_Atlas.m_pAtlasTexture, &srvDesc, m_pSrvHeap->GetCPUDescriptorHandleForHeapStart());

        // 7. Create Command List
        m_pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_pCmdAllocators[0], nullptr, IID_PPV_ARGS(&m_pCmdList));
        m_pCmdList->Close();

        // 8. Create Fence
        m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_pFence));
        m_hFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

        // 9. Build Pipeline State (Root Sig + Shaders)
        if (!BuildPipeline()) {
            return false;
        }

        // 10. Create Instance Buffer
        D3D12_RESOURCE_DESC instBufDesc = {};
        instBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        instBufDesc.Width = sizeof(GpuInstance) * MAX_INSTANCES_PER_FRAME;
        instBufDesc.Height = 1;
        instBufDesc.DepthOrArraySize = 1;
        instBufDesc.MipLevels = 1;
        instBufDesc.Format = DXGI_FORMAT_UNKNOWN;
        instBufDesc.SampleDesc.Count = 1;
        instBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        m_pDevice->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &instBufDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pInstanceBuffer));
        m_pInstanceBuffer->Map(0, nullptr, reinterpret_cast<void**>(&m_pMappedInstances));

        m_WindowState.alive = 1;
        m_WindowState.width = width;
        m_WindowState.height = height;

        return true;
    }

    bool BuildPipeline() {
        // Compile Shaders
        ID3DBlob* pVSBlob = nullptr;
        ID3DBlob* pPSBlob = nullptr;
        ID3DBlob* pErrorBlob = nullptr;

        HRESULT hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), "QuadShaders", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &pVSBlob, &pErrorBlob);
        if (FAILED(hr)) {
            if (pErrorBlob) { g_LastError = (char*)pErrorBlob->GetBufferPointer(); pErrorBlob->Release(); }
            return false;
        }

        hr = D3DCompile(g_ShaderSource, strlen(g_ShaderSource), "QuadShaders", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &pPSBlob, &pErrorBlob);
        if (FAILED(hr)) {
            if (pErrorBlob) { g_LastError = (char*)pErrorBlob->GetBufferPointer(); pErrorBlob->Release(); }
            if (pVSBlob) pVSBlob->Release();
            return false;
        }

        // Root Signature: 1 Root Constant (b0) + 1 Descriptor Table (t0) + 1 Static Sampler
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 2;
        rootSigDesc.pParameters = params;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &sampler;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* pSignatureBlob = nullptr;
        D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &pSignatureBlob, nullptr);
        m_pDevice->CreateRootSignature(0, pSignatureBlob->GetBufferPointer(), pSignatureBlob->GetBufferSize(), IID_PPV_ARGS(&m_pRootSignature));
        pSignatureBlob->Release();

        // Input Layout for Instanced Quads
        D3D12_INPUT_ELEMENT_DESC inputElements[] = {
            { "POSITION",  0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(GpuInstance, posX),        D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "SIZE",      0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(GpuInstance, sizeX),       D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TEXCOORD",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuInstance, u0),          D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "COLOR",     0, DXGI_FORMAT_R32_UINT,           0, offsetof(GpuInstance, color),       D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "RADIUS",    0, DXGI_FORMAT_R32_FLOAT,          0, offsetof(GpuInstance, radius),      D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "STROKEW",   0, DXGI_FORMAT_R32_FLOAT,          0, offsetof(GpuInstance, strokeW),     D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "MODE",      0, DXGI_FORMAT_R32_UINT,           0, offsetof(GpuInstance, mode),        D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
            { "TRANSFORM", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(GpuInstance, transOx),     D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputElements, _countof(inputElements) };
        psoDesc.pRootSignature = m_pRootSignature;
        psoDesc.VS = { pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize() };
        psoDesc.PS = { pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize() };
        
        // Alpha Blending
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        
        D3D12_RENDER_TARGET_BLEND_DESC blendDesc = {};
        blendDesc.BlendEnable = TRUE;
        blendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.BlendState.RenderTarget[0] = blendDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        psoDesc.SampleDesc.Count = 1;

        hr = m_pDevice->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pPSO));
        pVSBlob->Release();
        pPSBlob->Release();

        return SUCCEEDED(hr);
    }

    bool BeginFrame(uint32_t clearArgb) {
        if (!m_pCmdAllocators[m_FrameIndex] || !m_pCmdList) return false;

        m_pCmdAllocators[m_FrameIndex]->Reset();
        m_pCmdList->Reset(m_pCmdAllocators[m_FrameIndex], m_pPSO);

        // Upload any newly baked glyph tiles to GPU Atlas
        m_Atlas.FlushUploads(m_pCmdList);

        // Transition backbuffer to RENDER_TARGET
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_pRenderTargets[m_FrameIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCmdList->ResourceBarrier(1, &barrier);

        uint32_t rtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvHandle.ptr += m_FrameIndex * rtvDescriptorSize;

        float a = ((clearArgb >> 24) & 0xFF) / 255.0f;
        float r = ((clearArgb >> 16) & 0xFF) / 255.0f;
        float g = ((clearArgb >> 8)  & 0xFF) / 255.0f;
        float b = (clearArgb & 0xFF) / 255.0f;
        float clearColor[4] = { r, g, b, a };
        m_pCmdList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        m_pCmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        // Viewport & Scissor
        D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)m_Width, (float)m_Height, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)m_Width, (LONG)m_Height };
        m_pCmdList->RSSetViewports(1, &vp);
        m_pCmdList->RSSetScissorRects(1, &scissor);

        m_pCmdList->SetGraphicsRootSignature(m_pRootSignature);
        ID3D12DescriptorHeap* heaps[] = { m_pSrvHeap };
        m_pCmdList->SetDescriptorHeaps(1, heaps);
        m_pCmdList->SetGraphicsRootDescriptorTable(1, m_pSrvHeap->GetGPUDescriptorHandleForHeapStart());

        // Root Constants
        float rootConstants[4] = { (float)m_Width, (float)m_Height, 0.0f, 0.0f };
        m_pCmdList->SetGraphicsRoot32BitConstants(0, 4, rootConstants, 0);

        m_pCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        D3D12_VERTEX_BUFFER_VIEW vbView = {};
        vbView.BufferLocation = m_pInstanceBuffer->GetGPUVirtualAddress();
        vbView.StrideInBytes = sizeof(GpuInstance);
        vbView.SizeInBytes = sizeof(GpuInstance) * MAX_INSTANCES_PER_FRAME;
        m_pCmdList->IASetVertexBuffers(0, 1, &vbView);

        m_InstanceCount = 0;
        m_InstanceCursor = 0;
        m_BatchStart = 0;
        m_ClipStack.clear();
        m_TransformStack.clear();
        m_CurrentOpacity = 1.0f;

        return true;
    }

    void FlushInstances() {
        if (m_InstanceCount == 0 || !m_pCmdList) return;
        m_Atlas.FlushUploads(m_pCmdList);
        m_pCmdList->DrawInstanced(4, m_InstanceCount, 0, m_BatchStart);
        m_BatchStart = m_InstanceCursor;
        m_InstanceCount = 0;
    }

    void EmitInstance(GpuInstance inst) {
        if (m_CurrentOpacity < 0.999f) {
            uint32_t a = (uint32_t)(((inst.color >> 24) & 0xFF) * m_CurrentOpacity);
            inst.color = (a << 24) | (inst.color & 0x00FFFFFF);
        }
        if (m_InstanceCursor >= MAX_INSTANCES_PER_FRAME) {
            return; // diagnostic policy for now: NEVER WRAP
        }
        m_pMappedInstances[m_InstanceCursor++] = inst;
        ++m_InstanceCount;
    }

    void GetCurrentTransform(float* ox, float* oy, float* scale) {
        if (m_TransformStack.empty()) {
            *ox = 0.0f; *oy = 0.0f; *scale = 1.0f;
        } else {
            const auto& t = m_TransformStack.back();
            *ox = t.ox; *oy = t.oy; *scale = t.scale;
        }
    }

    void FillRect(float x, float y, float w, float h, uint32_t argb, float radius) {
        float ox, oy, scale;
        GetCurrentTransform(&ox, &oy, &scale);

        GpuInstance inst = {};
        inst.posX = x; inst.posY = y;
        inst.sizeX = w; inst.sizeY = h;
        inst.color = argb;
        inst.radius = radius;
        inst.strokeW = 0.0f;
        inst.mode = 0;
        inst.transOx = ox; inst.transOy = oy; inst.transScale = scale;
        EmitInstance(inst);
    }

    void DrawRect(float x, float y, float w, float h, uint32_t argb, float strokeW, float radius) {
        float ox, oy, scale;
        GetCurrentTransform(&ox, &oy, &scale);

        GpuInstance inst = {};
        inst.posX = x; inst.posY = y;
        inst.sizeX = w; inst.sizeY = h;
        inst.color = argb;
        inst.radius = radius;
        inst.strokeW = strokeW > 0.0f ? strokeW : 1.0f;
        inst.mode = 0;
        inst.transOx = ox; inst.transOy = oy; inst.transScale = scale;
        EmitInstance(inst);
    }

    void DrawLine(float x1, float y1, float x2, float y2, uint32_t argb, float strokeW) {
        float dx = x2 - x1;
        float dy = y2 - y1;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 0.001f) return;

        float ox, oy, scale;
        GetCurrentTransform(&ox, &oy, &scale);

        float sw = strokeW > 0.0f ? strokeW : 1.0f;
        GpuInstance inst = {};
        inst.posX = std::min(x1, x2); inst.posY = std::min(y1, y2);
        inst.sizeX = std::max(fabsf(dx), sw);
        inst.sizeY = std::max(fabsf(dy), sw);
        inst.color = argb;
        inst.strokeW = sw;
        inst.mode = 3;
        inst.transOx = ox; inst.transOy = oy; inst.transScale = scale;
        EmitInstance(inst);
    }

    void DrawShadow(float x, float y, float w, float h, float radius, float blur, float offsetY, uint32_t argb) {
        float ox, oy, scale;
        GetCurrentTransform(&ox, &oy, &scale);

        float pad = blur * 2.0f;
        GpuInstance inst = {};
        inst.posX = x - pad;
        inst.posY = y + offsetY - pad;
        inst.sizeX = w + pad * 2.0f;
        inst.sizeY = h + pad * 2.0f;
        inst.color = argb;
        inst.radius = radius;
        inst.strokeW = blur;
        inst.mode = 1;
        inst.transOx = ox; inst.transOy = oy; inst.transScale = scale;
        EmitInstance(inst);
    }

    void DrawTextClusters(const wchar_t* text, float x, float y, float maxW, float maxH, uint32_t argb, float fontSize, const wchar_t* fontFamily, bool bold, int32_t align) {
        if (!text || !text[0]) return;

        float ox, oy, scale;
        GetCurrentTransform(&ox, &oy, &scale);

        float curX = x;
        float curY = y;
        size_t len = wcslen(text);

        for (size_t i = 0; i < len; ++i) {
            wchar_t ch = text[i];
            if (ch == L'\n') {
                curX = x;
                curY += fontSize * 1.3f;
                continue;
            }
            if (ch == L'\r') continue;

            TileResource tile = m_Atlas.GetOrBakeGlyph(ch, fontSize, fontFamily, bold);
            if (tile.id > 0) {
                GpuInstance inst = {};
                inst.posX = curX;
                inst.posY = curY;
                inst.sizeX = tile.width;
                inst.sizeY = tile.height;
                inst.u0 = tile.u0; inst.v0 = tile.v0;
                inst.u1 = tile.u1; inst.v1 = tile.v1;
                inst.color = argb;
                inst.mode = 2; // Textured Glyph Tile
                inst.transOx = ox; inst.transOy = oy; inst.transScale = scale;
                EmitInstance(inst);

                curX += tile.advanceX;
                if (maxW > 0.0f && (curX - x) > maxW) {
                    break;
                }
            }
        }
    }

    bool MeasureText(const wchar_t* text, float fontSize, const wchar_t* fontFamily, bool bold, float* outW, float* outH) {
        if (!text || !text[0]) {
            *outW = 0.0f; *outH = fontSize * 1.2f;
            return true;
        }

        float totalW = 0.0f;
        size_t len = wcslen(text);
        for (size_t i = 0; i < len; ++i) {
            wchar_t ch = text[i];
            if (ch == L'\r' || ch == L'\n') continue;
            TileResource tile = m_Atlas.GetOrBakeGlyph(ch, fontSize, fontFamily, bold);
            totalW += tile.advanceX;
        }
        *outW = totalW;
        *outH = fontSize * 1.25f;
        return true;
    }

    void PushClip(float x, float y, float w, float h) {
        FlushInstances();
        float ox, oy, scale;
        GetCurrentTransform(&ox, &oy, &scale);

        float worldX = x * scale + ox;
        float worldY = y * scale + oy;
        float worldR = (x + w) * scale + ox;
        float worldB = (y + h) * scale + oy;

        if (!m_ClipStack.empty()) {
            const auto& parent = m_ClipStack.back();
            worldX = std::max(parent.x, worldX);
            worldY = std::max(parent.y, worldY);
            worldR = std::min(parent.x + parent.w, worldR);
            worldB = std::min(parent.y + parent.h, worldB);
        }

        ScissorClip clip = { worldX, worldY, std::max(0.0f, worldR - worldX), std::max(0.0f, worldB - worldY) };
        m_ClipStack.push_back(clip);

        LONG rLeft = (LONG)std::max(0.0f, std::min((float)m_Width, clip.x));
        LONG rTop = (LONG)std::max(0.0f, std::min((float)m_Height, clip.y));
        LONG rRight = (LONG)std::max((float)rLeft, std::min((float)m_Width, clip.x + clip.w));
        LONG rBottom = (LONG)std::max((float)rTop, std::min((float)m_Height, clip.y + clip.h));

        D3D12_RECT rect = { rLeft, rTop, rRight, rBottom };
        m_pCmdList->RSSetScissorRects(1, &rect);
    }

    void PopClip() {
        FlushInstances();
        if (!m_ClipStack.empty()) m_ClipStack.pop_back();

        if (m_ClipStack.empty()) {
            D3D12_RECT rect = { 0, 0, (LONG)m_Width, (LONG)m_Height };
            m_pCmdList->RSSetScissorRects(1, &rect);
        } else {
            const auto& clip = m_ClipStack.back();
            LONG rLeft = (LONG)std::max(0.0f, std::min((float)m_Width, clip.x));
            LONG rTop = (LONG)std::max(0.0f, std::min((float)m_Height, clip.y));
            LONG rRight = (LONG)std::max((float)rLeft, std::min((float)m_Width, clip.x + clip.w));
            LONG rBottom = (LONG)std::max((float)rTop, std::min((float)m_Height, clip.y + clip.h));

            D3D12_RECT rect = { rLeft, rTop, rRight, rBottom };
            m_pCmdList->RSSetScissorRects(1, &rect);
        }
    }

    void PushTransform(float ox, float oy, float scale) {
        m_TransformStack.push_back({ ox, oy, scale });
    }

    void PopTransform() {
        if (!m_TransformStack.empty()) m_TransformStack.pop_back();
    }

    void SetOpacity(float alpha) {
        m_CurrentOpacity = alpha;
    }

    void DrawWireIcon(const wchar_t* name, float x, float y, float size, uint32_t argb, float strokeW) {
        if (!name) return;
        float s = size > 0.0f ? size : 16.0f;
        float sw = strokeW > 0.0f ? strokeW : 1.2f;

        if (wcscmp(name, L"new") == 0) {
            DrawLine(x + s * 0.25f, y + s * 0.5f, x + s * 0.75f, y + s * 0.5f, argb, sw);
            DrawLine(x + s * 0.5f, y + s * 0.25f, x + s * 0.5f, y + s * 0.75f, argb, sw);
        } else if (wcscmp(name, L"cut") == 0) {
            DrawRect(x + s * 0.2f, y + s * 0.6f, s * 0.25f, s * 0.25f, argb, sw, s * 0.125f);
            DrawRect(x + s * 0.55f, y + s * 0.6f, s * 0.25f, s * 0.25f, argb, sw, s * 0.125f);
            DrawLine(x + s * 0.3f, y + s * 0.6f, x + s * 0.7f, y + s * 0.2f, argb, sw);
            DrawLine(x + s * 0.7f, y + s * 0.6f, x + s * 0.3f, y + s * 0.2f, argb, sw);
        } else if (wcscmp(name, L"copy") == 0) {
            DrawRect(x + s * 0.2f, y + s * 0.35f, s * 0.45f, s * 0.5f, argb, sw, 1.0f);
            DrawRect(x + s * 0.38f, y + s * 0.18f, s * 0.45f, s * 0.5f, argb, sw, 1.0f);
        } else if (wcscmp(name, L"paste") == 0) {
            DrawRect(x + s * 0.25f, y + s * 0.3f, s * 0.5f, s * 0.55f, argb, sw, 1.0f);
            DrawRect(x + s * 0.38f, y + s * 0.18f, s * 0.24f, s * 0.16f, argb, sw, 1.0f);
            DrawLine(x + s * 0.22f, y + s * 0.3f, x + s * 0.78f, y + s * 0.3f, argb, sw);
        } else if (wcscmp(name, L"del") == 0) {
            DrawRect(x + s * 0.28f, y + s * 0.35f, s * 0.44f, s * 0.52f, argb, sw, 1.0f);
            DrawLine(x + s * 0.18f, y + s * 0.28f, x + s * 0.82f, y + s * 0.28f, argb, sw);
            DrawLine(x + s * 0.38f, y + s * 0.28f, x + s * 0.38f, y + s * 0.18f, argb, sw);
            DrawLine(x + s * 0.38f, y + s * 0.18f, x + s * 0.62f, y + s * 0.18f, argb, sw);
            DrawLine(x + s * 0.62f, y + s * 0.18f, x + s * 0.62f, y + s * 0.28f, argb, sw);
        } else if (wcscmp(name, L"view") == 0) {
            DrawRect(x + s * 0.18f, y + s * 0.22f, s * 0.64f, s * 0.56f, argb, sw, 1.0f);
            DrawLine(x + s * 0.18f, y + s * 0.4f, x + s * 0.82f, y + s * 0.4f, argb, sw);
        } else if (wcscmp(name, L"details") == 0) {
            DrawLine(x + s * 0.2f, y + s * 0.28f, x + s * 0.8f, y + s * 0.28f, argb, sw);
            DrawLine(x + s * 0.2f, y + s * 0.5f, x + s * 0.8f, y + s * 0.5f, argb, sw);
            DrawLine(x + s * 0.2f, y + s * 0.72f, x + s * 0.8f, y + s * 0.72f, argb, sw);
        } else if (wcscmp(name, L"large") == 0) {
            float q = s * 0.25f;
            DrawRect(x + s * 0.18f, y + s * 0.18f, q, q, argb, sw, 1.0f);
            DrawRect(x + s * 0.57f, y + s * 0.18f, q, q, argb, sw, 1.0f);
            DrawRect(x + s * 0.18f, y + s * 0.57f, q, q, argb, sw, 1.0f);
            DrawRect(x + s * 0.57f, y + s * 0.57f, q, q, argb, sw, 1.0f);
        } else if (wcscmp(name, L"pin") == 0) {
            DrawLine(x + s * 0.3f, y + s * 0.1f, x + s * 0.7f, y + s * 0.1f, argb, sw);
            DrawLine(x + s * 0.5f, y + s * 0.1f, x + s * 0.5f, y + s * 0.7f, argb, sw);
            DrawLine(x + s * 0.2f, y + s * 0.7f, x + s * 0.8f, y + s * 0.7f, argb, sw);
            DrawLine(x + s * 0.5f, y + s * 0.7f, x + s * 0.5f, y + s * 0.95f, argb, sw);
        } else if (wcscmp(name, L"search") == 0) {
            DrawRect(x + 2.0f, y + 2.0f, s - 6.0f, s - 6.0f, argb, sw, (s - 6.0f) * 0.5f);
            DrawLine(x + s - 6.0f, y + s - 6.0f, x + s - 1.0f, y + s - 1.0f, argb, sw);
        } else if (wcscmp(name, L"win_minimize") == 0) {
            DrawLine(x + s * 0.2f, y + s * 0.5f, x + s * 0.8f, y + s * 0.5f, argb, sw);
        } else if (wcscmp(name, L"win_maximize") == 0) {
            DrawRect(x + s * 0.2f, y + s * 0.2f, s * 0.6f, s * 0.6f, argb, sw, 0.0f);
        } else if (wcscmp(name, L"win_close") == 0 || wcscmp(name, L"tab_close") == 0) {
            DrawLine(x + s * 0.2f, y + s * 0.2f, x + s * 0.8f, y + s * 0.8f, argb, sw);
            DrawLine(x + s * 0.8f, y + s * 0.2f, x + s * 0.2f, y + s * 0.8f, argb, sw);
        } else if (wcscmp(name, L"tab_add") == 0) {
            DrawLine(x + s * 0.2f, y + s * 0.5f, x + s * 0.8f, y + s * 0.5f, argb, sw);
            DrawLine(x + s * 0.5f, y + s * 0.2f, x + s * 0.5f, y + s * 0.8f, argb, sw);
        } else {
            DrawRect(x, y, s, s, argb, sw, 2.0f);
        }
    }

    bool EndFrame() {
        FlushInstances();

        // Transition backbuffer to PRESENT
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = m_pRenderTargets[m_FrameIndex];
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCmdList->ResourceBarrier(1, &barrier);

        m_pCmdList->Close();

        ID3D12CommandList* lists[] = { m_pCmdList };
        m_pCommandQueue->ExecuteCommandLists(1, lists);

        m_pSwapChain->Present(1, 0);

        // Signal fence
        const uint64_t fenceVal = ++m_FenceValues[m_FrameIndex];
        m_pCommandQueue->Signal(m_pFence, fenceVal);

        m_FrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();
        if (m_pFence->GetCompletedValue() < m_FenceValues[m_FrameIndex]) {
            m_pFence->SetEventOnCompletion(m_FenceValues[m_FrameIndex], m_hFenceEvent);
            WaitForSingleObject(m_hFenceEvent, INFINITE);
        }

        return true;
    }

    void Resize(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0 || (width == m_Width && height == m_Height)) return;
        if (!m_pSwapChain || !m_pDevice || !m_pCommandQueue) return;

        // Flush GPU before releasing buffers
        const uint64_t fenceVal = ++m_FenceValues[m_FrameIndex];
        m_pCommandQueue->Signal(m_pFence, fenceVal);
        m_pFence->SetEventOnCompletion(fenceVal, m_hFenceEvent);
        WaitForSingleObject(m_hFenceEvent, INFINITE);

        for (uint32_t i = 0; i < 2; i++) {
            if (m_pRenderTargets[i]) {
                m_pRenderTargets[i]->Release();
                m_pRenderTargets[i] = nullptr;
            }
        }

        m_Width = width;
        m_Height = height;
        m_pSwapChain->ResizeBuffers(2, m_Width, m_Height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        m_FrameIndex = m_pSwapChain->GetCurrentBackBufferIndex();

        uint32_t rtvDescriptorSize = m_pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_pRtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (uint32_t i = 0; i < 2; i++) {
            m_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&m_pRenderTargets[i]));
            m_pDevice->CreateRenderTargetView(m_pRenderTargets[i], nullptr, rtvHandle);
            rtvHandle.ptr += rtvDescriptorSize;
        }

        m_WindowState.width = width;
        m_WindowState.height = height;
        m_WindowState.resize_serial++;
    }

    bool SavePng(const wchar_t* filePath) {
        if (!filePath || !m_pDevice || !m_pCommandQueue) return false;

        // Target the backbuffer that was just presented
        uint32_t targetIdx = m_FrameIndex ^ 1;
        ID3D12Resource* pSourceTarget = m_pRenderTargets[targetIdx];
        if (!pSourceTarget) return false;

        const uint64_t fenceVal = ++m_FenceValues[m_FrameIndex];
        m_pCommandQueue->Signal(m_pFence, fenceVal);
        m_pFence->SetEventOnCompletion(fenceVal, m_hFenceEvent);
        WaitForSingleObject(m_hFenceEvent, INFINITE);

        uint32_t rowPitch = (m_Width * 4 + 255) & ~255;
        uint64_t readbackSize = (uint64_t)rowPitch * m_Height;

        D3D12_RESOURCE_DESC rbDesc = {};
        rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbDesc.Width = readbackSize;
        rbDesc.Height = 1;
        rbDesc.DepthOrArraySize = 1;
        rbDesc.MipLevels = 1;
        rbDesc.Format = DXGI_FORMAT_UNKNOWN;
        rbDesc.SampleDesc.Count = 1;
        rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12_HEAP_PROPERTIES readbackHeap = {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

        ID3D12Resource* pReadbackBuffer = nullptr;
        HRESULT hr = m_pDevice->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&pReadbackBuffer));
        if (FAILED(hr)) return false;

        m_pCmdAllocators[m_FrameIndex]->Reset();
        m_pCmdList->Reset(m_pCmdAllocators[m_FrameIndex], nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = pSourceTarget;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_pCmdList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = pReadbackBuffer;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = m_Width;
        dst.PlacedFootprint.Footprint.Height = m_Height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = pSourceTarget;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;

        m_pCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        m_pCmdList->ResourceBarrier(1, &barrier);

        m_pCmdList->Close();
        ID3D12CommandList* lists[] = { m_pCmdList };
        m_pCommandQueue->ExecuteCommandLists(1, lists);

        const uint64_t copyFence = ++m_FenceValues[m_FrameIndex];
        m_pCommandQueue->Signal(m_pFence, copyFence);
        m_pFence->SetEventOnCompletion(copyFence, m_hFenceEvent);
        WaitForSingleObject(m_hFenceEvent, INFINITE);

        uint8_t* pMapped = nullptr;
        pReadbackBuffer->Map(0, nullptr, reinterpret_cast<void**>(&pMapped));

        bool success = false;
        IWICImagingFactory* pWICFactory = nullptr;
        CoInitialize(nullptr);
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pWICFactory));
        if (SUCCEEDED(hr) && pWICFactory) {
            IWICStream* pStream = nullptr;
            hr = pWICFactory->CreateStream(&pStream);
            if (SUCCEEDED(hr) && pStream) {
                hr = pStream->InitializeFromFilename(filePath, GENERIC_WRITE);
                if (SUCCEEDED(hr)) {
                    IWICBitmapEncoder* pEncoder = nullptr;
                    hr = pWICFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &pEncoder);
                    if (SUCCEEDED(hr) && pEncoder) {
                        hr = pEncoder->Initialize(pStream, WICBitmapEncoderNoCache);
                        if (SUCCEEDED(hr)) {
                            IWICBitmapFrameEncode* pFrame = nullptr;
                            hr = pEncoder->CreateNewFrame(&pFrame, nullptr);
                            if (SUCCEEDED(hr) && pFrame) {
                                hr = pFrame->Initialize(nullptr);
                                pFrame->SetSize(m_Width, m_Height);
                                WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
                                pFrame->SetPixelFormat(&format);
                                pFrame->WritePixels(m_Height, rowPitch, (UINT)readbackSize, pMapped);
                                pFrame->Commit();
                                pFrame->Release();
                                pEncoder->Commit();
                                success = true;
                            }
                        }
                        pEncoder->Release();
                    }
                }
                pStream->Release();
            }
            pWICFactory->Release();
        }

        pReadbackBuffer->Unmap(0, nullptr);
        pReadbackBuffer->Release();

        return success;
    }

    void SetCursor(DP2D_CURSOR cursor) {
        if (cursor >= 0 && cursor < 6 && m_Cursors[cursor]) {
            ::SetCursor(m_Cursors[cursor]);
        }
    }

    bool PumpWindow(uint32_t waitMs, DP2D_WINDOW_STATE* outState) {
        if (waitMs > 0) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, waitMs, QS_ALLINPUT);
        }

        MSG msg;
        m_WindowState.char_code = 0;
        m_WindowState.wheel_delta = 0;
        m_WindowState.is_double_click = 0;

        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_WindowState.alive = 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (outState) {
            *outState = m_WindowState;
        }
        return m_WindowState.alive != 0;
    }

    static LRESULT CALLBACK WndProcStatic(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        D3D12Canvas* pThis = nullptr;
        if (msg == WM_NCCREATE) {
            auto* pCreate = reinterpret_cast<CREATESTRUCTW*>(lParam);
            pThis = reinterpret_cast<D3D12Canvas*>(pCreate->lpCreateParams);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        } else {
            pThis = reinterpret_cast<D3D12Canvas*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        }

        if (pThis) {
            return pThis->WndProc(hWnd, msg, wParam, lParam);
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    LRESULT WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                Resize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;

        case WM_MOUSEMOVE:
            m_WindowState.mouse_x = (int)(short)LOWORD(lParam);
            m_WindowState.mouse_y = (int)(short)HIWORD(lParam);
            return 0;

        case WM_LBUTTONDOWN:
            SetCapture(hWnd);
            m_WindowState.left_down = 1;
            m_WindowState.mouse_x = (int)(short)LOWORD(lParam);
            m_WindowState.mouse_y = (int)(short)HIWORD(lParam);
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            m_WindowState.left_down = 0;
            m_WindowState.mouse_x = (int)(short)LOWORD(lParam);
            m_WindowState.mouse_y = (int)(short)HIWORD(lParam);
            return 0;

        case WM_LBUTTONDBLCLK:
            m_WindowState.is_double_click = 1;
            return 0;

        case WM_RBUTTONDOWN:
            m_WindowState.right_down = 1;
            return 0;

        case WM_RBUTTONUP:
            m_WindowState.right_down = 0;
            return 0;

        case WM_MOUSEWHEEL:
            m_WindowState.wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
            return 0;

        case WM_KEYDOWN:
            m_WindowState.key_code = (int32_t)wParam;
            return 0;

        case WM_CHAR:
            m_WindowState.char_code = (wchar_t)wParam;
            return 0;

        case WM_DESTROY:
            m_WindowState.alive = 0;
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void Shutdown() {
        if (m_pFence && m_pCommandQueue) {
            uint64_t val = ++m_FenceValues[m_FrameIndex];
            m_pCommandQueue->Signal(m_pFence, val);
            if (m_pFence->GetCompletedValue() < val) {
                m_pFence->SetEventOnCompletion(val, m_hFenceEvent);
                WaitForSingleObject(m_hFenceEvent, INFINITE);
            }
        }

        m_Atlas.Shutdown();

        if (m_pInstanceBuffer) {
            m_pInstanceBuffer->Unmap(0, nullptr);
            m_pInstanceBuffer->Release();
            m_pInstanceBuffer = nullptr;
        }
        if (m_pPSO) { m_pPSO->Release(); m_pPSO = nullptr; }
        if (m_pRootSignature) { m_pRootSignature->Release(); m_pRootSignature = nullptr; }
        if (m_pCmdList) { m_pCmdList->Release(); m_pCmdList = nullptr; }
        for (int i = 0; i < 2; i++) {
            if (m_pRenderTargets[i]) { m_pRenderTargets[i]->Release(); m_pRenderTargets[i] = nullptr; }
            if (m_pCmdAllocators[i]) { m_pCmdAllocators[i]->Release(); m_pCmdAllocators[i] = nullptr; }
        }
        if (m_pRtvHeap) { m_pRtvHeap->Release(); m_pRtvHeap = nullptr; }
        if (m_pSrvHeap) { m_pSrvHeap->Release(); m_pSrvHeap = nullptr; }
        if (m_pFence) { m_pFence->Release(); m_pFence = nullptr; }
        if (m_hFenceEvent) { CloseHandle(m_hFenceEvent); m_hFenceEvent = nullptr; }
        if (m_pSwapChain) { m_pSwapChain->Release(); m_pSwapChain = nullptr; }
        if (m_pCommandQueue) { m_pCommandQueue->Release(); m_pCommandQueue = nullptr; }
        if (m_pDevice) { m_pDevice->Release(); m_pDevice = nullptr; }

        if (m_hWnd) {
            DestroyWindow(m_hWnd);
            m_hWnd = nullptr;
        }
    }
};

} // namespace

// ============================================================================
// C EXPORTS
// ============================================================================

extern "C" {

DP2D_EXPORT DP2D_HANDLE dp2d_create(uint32_t width, uint32_t height, const wchar_t* title) {
    auto* pCanvas = new D3D12Canvas();
    if (!pCanvas->Initialize(width, height, title)) {
        delete pCanvas;
        return nullptr;
    }
    return reinterpret_cast<DP2D_HANDLE>(pCanvas);
}

DP2D_EXPORT bool dp2d_pump_window(DP2D_HANDLE handle, uint32_t wait_ms, DP2D_WINDOW_STATE* state) {
    if (!handle) return false;
    return reinterpret_cast<D3D12Canvas*>(handle)->PumpWindow(wait_ms, state);
}

DP2D_EXPORT void dp2d_set_cursor(DP2D_HANDLE handle, DP2D_CURSOR cursor) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->SetCursor(cursor);
}

DP2D_EXPORT bool dp2d_begin_frame(DP2D_HANDLE handle, uint32_t clear_color_argb) {
    if (!handle) return false;
    return reinterpret_cast<D3D12Canvas*>(handle)->BeginFrame(clear_color_argb);
}

DP2D_EXPORT void dp2d_fill_rect(DP2D_HANDLE handle, float x, float y, float w, float h, uint32_t argb, float radius) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->FillRect(x, y, w, h, argb, radius);
}

DP2D_EXPORT void dp2d_draw_rect(DP2D_HANDLE handle, float x, float y, float w, float h, uint32_t argb, float stroke_width, float radius) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->DrawRect(x, y, w, h, argb, stroke_width, radius);
}

DP2D_EXPORT void dp2d_draw_line(DP2D_HANDLE handle, float x1, float y1, float x2, float y2, uint32_t argb, float stroke_width) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->DrawLine(x1, y1, x2, y2, argb, stroke_width);
}

DP2D_EXPORT void dp2d_draw_text(DP2D_HANDLE handle, const wchar_t* text, float x, float y, float max_width, float max_height, uint32_t argb, float font_size, const wchar_t* font_family, bool bold, int32_t alignment) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->DrawTextClusters(text, x, y, max_width, max_height, argb, font_size, font_family, bold, alignment);
}

DP2D_EXPORT bool dp2d_measure_text(DP2D_HANDLE handle, const wchar_t* text, float font_size, const wchar_t* font_family, bool bold, float* out_width, float* out_height) {
    if (!handle) return false;
    return reinterpret_cast<D3D12Canvas*>(handle)->MeasureText(text, font_size, font_family, bold, out_width, out_height);
}

DP2D_EXPORT void dp2d_push_clip(DP2D_HANDLE handle, float x, float y, float w, float h) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->PushClip(x, y, w, h);
}

DP2D_EXPORT void dp2d_pop_clip(DP2D_HANDLE handle) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->PopClip();
}

DP2D_EXPORT void dp2d_push_transform(DP2D_HANDLE handle, float offset_x, float offset_y, float scale) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->PushTransform(offset_x, offset_y, scale);
}

DP2D_EXPORT void dp2d_pop_transform(DP2D_HANDLE handle) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->PopTransform();
}

DP2D_EXPORT void dp2d_set_opacity(DP2D_HANDLE handle, float alpha) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->SetOpacity(alpha);
}

DP2D_EXPORT void dp2d_draw_shadow(DP2D_HANDLE handle, float x, float y, float w, float h, float radius, float blur, float offset_y, uint32_t argb) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->DrawShadow(x, y, w, h, radius, blur, offset_y, argb);
}

DP2D_EXPORT void dp2d_draw_wire_icon(DP2D_HANDLE handle, const wchar_t* icon_name, float x, float y, float size, uint32_t argb, float stroke_width) {
    if (!handle) return;
    reinterpret_cast<D3D12Canvas*>(handle)->DrawWireIcon(icon_name, x, y, size, argb, stroke_width);
}

DP2D_EXPORT bool dp2d_end_frame(DP2D_HANDLE handle) {
    if (!handle) return false;
    return reinterpret_cast<D3D12Canvas*>(handle)->EndFrame();
}

DP2D_EXPORT bool dp2d_save_png(DP2D_HANDLE handle, const wchar_t* file_path) {
    if (!handle || !file_path) return false;
    return reinterpret_cast<D3D12Canvas*>(handle)->SavePng(file_path);
}

DP2D_EXPORT const char* dp2d_last_error(void) {
    return g_LastError.c_str();
}

DP2D_EXPORT void dp2d_close(DP2D_HANDLE handle) {
    if (!handle) return;
    auto* pCanvas = reinterpret_cast<D3D12Canvas*>(handle);
    pCanvas->Shutdown();
    delete pCanvas;
}

} // extern "C"
