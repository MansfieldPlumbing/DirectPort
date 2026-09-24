// --- DirectPortProducer.cpp ---
// High-performance D3D12 DirectPort Producer.
// Supports:
//  - Dynamic HLSL shader loading & F5 hot-reload
//  - Precompiled CSO bytecode loading
//  - Live Media Foundation camera stream capture
//  - WASAPI loopback audio stream capture (synchronized A/V crossbar)
//  - Windows 11 Mica Alt backdrop
//  - Non-blocking UDP discovery beacon (dual-mode loopback/LAN)
//  - Event-driven waitable swapchain pump (zero polling thrash)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <vector>
#include <fstream>
#include "../sdk/directport.h"
#include "../sdk/DirectPort_Discovery.h"
#include "../sdk/DirectPort_Audio.h"
#include "../sdk/DirectPort_Camera.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Microsoft::WRL;

// --- DWM Mica Window Attributes (from QuickPS) ---
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

static const UINT kFrameCount = 2;
static const UINT kDefaultWidth = 1920;
static const UINT kDefaultHeight = 1080;

struct ShaderConstants {
    float u_resolution[4]; // xy = res, z = aspect, w = 0
    float u_time[4];       // x = elapsed, y = delta, z = frameIndex, w = 0
    float u_mouse[4];      // xy = pos, zw = 0
};

// --- Subsystem Handles ---
static HWND                           g_hwnd = nullptr;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_allocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_sharedRtvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_pipelineState;
static ComPtr<ID3D12Resource>         g_constantBuffer;
static ShaderConstants*               g_pCbvData = nullptr;
static UINT                           g_rtvDescriptorSize = 0;
static UINT                           g_frameIndex = 0;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_fenceValues[kFrameCount] = {};
static HANDLE                         g_fenceEvent = nullptr;

// --- DirectPort Sharing Primitives ---
static ComPtr<ID3D12Resource>         g_sharedTexture;
static ComPtr<ID3D12Fence>            g_sharedFence;
static UINT64                         g_sharedFrameValue = 0;
static HANDLE                         g_sharedTextureHandle = nullptr;
static HANDLE                         g_sharedFenceHandle = nullptr;
static std::wstring                   g_streamName = L"DirectPort_Main";
static std::wstring                   g_texHandleName = L"DirectPort_Tex_Main";
static std::wstring                   g_fenceHandleName = L"DirectPort_Fence_Main";
static std::wstring                   g_audioBufferName = L"DirectPort_Audio_Main";

// --- Network Discovery & A/V Services ---
static DirectPortDiscoveryBroadcaster g_broadcaster;
static DirectPortAudioRingProducer    g_audioRing;
static DirectPortWASAPICapture        g_wasapiCapture;
static DirectPortCameraCapture        g_cameraCapture;
static bool                           g_hasAudio = true;
static bool                           g_useCamera = false;
static std::string                    g_loadedShaderPath = "shaders/plasma.hlsl";
static auto                           g_startTime = std::chrono::steady_clock::now();

// --- Camera Upload Staging ---
static ComPtr<ID3D12Resource>         g_cameraUploadBuffer;
static UINT8*                         g_pCameraUploadData = nullptr;

// --- Forward Declarations ---
void ApplyMicaWindowAttributes(HWND hwnd);
bool InitD3D12(HWND hwnd);
bool LoadShaderPipeline(const std::string& shaderPath);
bool InitializeDirectPort(UINT width, UINT height);
void RenderFrame();
void MoveToNextFrame();
void WaitForGpuIdle();
void Cleanup();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--camera") == 0 || _wcsicmp(argv[i], L"--webcam") == 0) {
            g_useCamera = true;
        } else if (_wcsicmp(argv[i], L"--no-audio") == 0) {
            g_hasAudio = false;
        } else if (argv[i][0] != L'-') {
            char buf[512] = {};
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, buf, sizeof(buf), NULL, NULL);
            g_loadedShaderPath = buf;
        }
    }
    LocalFree(argv);

    const WCHAR szClass[] = L"DirectPortProducerClass";
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = szClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    std::wstring winTitle = L"DirectPort Producer // " + 
        std::wstring(g_useCamera ? L"Camera Feed" : L"HLSL Shader") + 
        (g_hasAudio ? L" + WASAPI Audio" : L"");

    g_hwnd = CreateWindowExW(0, szClass, winTitle.c_str(), 
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    ApplyMicaWindowAttributes(g_hwnd);

    if (!InitD3D12(g_hwnd)) return 1;

    if (g_useCamera) {
        if (!g_cameraCapture.Initialize(0)) {
            // Camera unavailable, fallback to shader
            g_useCamera = false;
        }
    }

    if (!g_useCamera) {
        if (!LoadShaderPipeline(g_loadedShaderPath)) {
            LoadShaderPipeline("shaders/smpte.hlsl");
        }
    }

    if (!InitializeDirectPort(kDefaultWidth, kDefaultHeight)) return 1;

    // Initialize WASAPI Audio capture ring
    if (g_hasAudio) {
        if (g_audioRing.Initialize(g_audioBufferName.c_str(), 48000, 2)) {
            g_wasapiCapture.Start(&g_audioRing);
        } else {
            g_hasAudio = false;
        }
    }

    g_broadcaster.Start();
    ShowWindow(g_hwnd, nCmdShow);

    // Event-driven message pump with waitable swapchain
    HANDLE hWaitable = g_swapChain->GetFrameLatencyWaitableObject();
    bool running = true;

    while (running) {
        DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &hWaitable, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        
        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (running && (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT)) {
            RenderFrame();
        }
    }

    Cleanup();
    return 0;
}

void ApplyMicaWindowAttributes(HWND hwnd) {
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    COLORREF captionColor = 0xFFFFFFFE; // Seamless dark titlebar
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    COLORREF textColor = 0x00FFFFFF; // White text
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));

    int backdrop = 4; // Mica Alt
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
        backdrop = 2; // Mica fallback
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    }
}

bool InitD3D12(HWND hwnd) {
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)))) return false;

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) return false;

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&g_commandQueue)))) return false;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT width = rc.right - rc.left;
    UINT height = rc.bottom - rc.top;

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1))) return false;
    swapChain1.As(&g_swapChain);
    g_swapChain->SetMaximumFrameLatency(1);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = kFrameCount;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_allocators[i]));
    }

    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // Constant buffer for uniforms
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = (sizeof(ShaderConstants) + 255) & ~255;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &cbDesc, 
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_constantBuffer));
    g_constantBuffer->Map(0, nullptr, reinterpret_cast<void**>(&g_pCbvData));

    // Staging buffer for camera frames (1920x1080x4)
    D3D12_RESOURCE_DESC camUploadDesc = {};
    camUploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    camUploadDesc.Width = kDefaultWidth * kDefaultHeight * 4;
    camUploadDesc.Height = 1;
    camUploadDesc.DepthOrArraySize = 1;
    camUploadDesc.MipLevels = 1;
    camUploadDesc.SampleDesc.Count = 1;
    camUploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &camUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cameraUploadBuffer));
    g_cameraUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&g_pCameraUploadData));

    return true;
}

bool LoadShaderPipeline(const std::string& shaderPath) {
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    bool isCso = (shaderPath.size() > 4 && shaderPath.substr(shaderPath.size() - 4) == ".cso");

    if (isCso) {
        // Read precompiled CSO bytecode directly
        std::ifstream file(shaderPath, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        D3DCreateBlob((SIZE_T)size, &psBlob);
        file.read(reinterpret_cast<char*>(psBlob->GetBufferPointer()), size);
    } else {
        std::wstring wPath(shaderPath.begin(), shaderPath.end());
        HRESULT hr = D3DCompileFromFile(wPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, 
            "VSMain", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) return false;

        hr = D3DCompileFromFile(wPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, 
            "PSMain", "ps_5_0", compileFlags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) return false;
    }

    // Default passthrough VS if CSO only had pixel shader
    if (!vsBlob) {
        const char* passthroughVS = 
            "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
            "VSOut VSMain(uint id : SV_VertexID) {\n"
            "    VSOut o;\n"
            "    o.uv = float2((id << 1) & 2, id & 2);\n"
            "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
            "    return o;\n"
            "}\n";
        D3DCompile(passthroughVS, strlen(passthroughVS), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    }

    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParam.Descriptor.ShaderRegister = 0;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, nullptr);
    g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    return SUCCEEDED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState)));
}

bool InitializeDirectPort(UINT width, UINT height) {
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (FAILED(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_SHARED, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, &clearVal, IID_PPV_ARGS(&g_sharedTexture)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.NumDescriptors = 1;
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_sharedRtvHeap));
    g_device->CreateRenderTargetView(g_sharedTexture.Get(), nullptr, g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedFence)))) return false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

    g_device->CreateSharedHandle(g_sharedTexture.Get(), &sa, GENERIC_ALL, g_texHandleName.c_str(), &g_sharedTextureHandle);
    g_device->CreateSharedHandle(g_sharedFence.Get(), &sa, GENERIC_ALL, g_fenceHandleName.c_str(), &g_sharedFenceHandle);

    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
    return true;
}

void RenderFrame() {
    g_allocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_allocators[g_frameIndex].Get(), g_pipelineState.Get());

    if (g_useCamera && g_cameraCapture.IsActive()) {
        std::vector<BYTE> camBytes;
        if (g_cameraCapture.ReadFrame(camBytes) && g_pCameraUploadData) {
            memcpy(g_pCameraUploadData, camBytes.data(), min(camBytes.size(), (size_t)(kDefaultWidth * kDefaultHeight * 4)));
        }

        // Copy from staging upload buffer to shared texture
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_sharedTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_commandList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = g_sharedTexture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = g_cameraUploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Width = kDefaultWidth;
        src.PlacedFootprint.Footprint.Height = kDefaultHeight;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = kDefaultWidth * 4;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        g_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &barrier);
    } else {
        // Procedural Shader Mode
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - g_startTime).count();

        g_pCbvData->u_resolution[0] = (float)kDefaultWidth;
        g_pCbvData->u_resolution[1] = (float)kDefaultHeight;
        g_pCbvData->u_resolution[2] = (float)kDefaultWidth / (float)kDefaultHeight;
        g_pCbvData->u_time[0] = elapsed;
        g_pCbvData->u_time[2] = (float)g_sharedFrameValue;

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_sharedTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)kDefaultWidth, (float)kDefaultHeight, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)kDefaultWidth, (LONG)kDefaultHeight };
        g_commandList->RSSetViewports(1, &vp);
        g_commandList->RSSetScissorRects(1, &scissor);

        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
        g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());

        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_commandList->DrawInstanced(3, 1, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &barrier);
    }

    // Blit to window backbuffer for local preview
    D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
    copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarriers[0].Transition.pResource = g_sharedTexture.Get();
    copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

    copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copyBarriers[1].Transition.pResource = g_renderTargets[g_frameIndex].Get();
    copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_commandList->ResourceBarrier(2, copyBarriers);

    g_commandList->CopyResource(g_renderTargets[g_frameIndex].Get(), g_sharedTexture.Get());

    copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(2, copyBarriers);

    g_commandList->Close();

    ID3D12CommandList* cmdLists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, cmdLists);

    // Hardware Fence Signal (Crossbar sync for consumer)
    g_sharedFrameValue++;
    g_commandQueue->Signal(g_sharedFence.Get(), g_sharedFrameValue);

    // Broadcast UDP discovery beacon immediately on start and once every 3 seconds (~180 frames)
    if (g_sharedFrameValue == 1 || g_sharedFrameValue % 180 == 0) {
        g_broadcaster.Broadcast("DirectPort_Main", kDefaultWidth, kDefaultHeight, DXGI_FORMAT_R8G8B8A8_UNORM, 
            g_texHandleName.c_str(), g_fenceHandleName.c_str(), g_sharedFrameValue,
            g_hasAudio, 48000, 2, g_audioBufferName.c_str());
    }

    g_swapChain->Present(1, 0);
    MoveToNextFrame();
}

void MoveToNextFrame() {
    UINT64 currentFence = g_fenceValues[g_frameIndex];
    g_commandQueue->Signal(g_renderFence.Get(), currentFence);

    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    if (g_renderFence->GetCompletedValue() < g_fenceValues[g_frameIndex]) {
        g_renderFence->SetEventOnCompletion(g_fenceValues[g_frameIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }

    g_fenceValues[g_frameIndex] = currentFence + 1;
}

void WaitForGpuIdle() {
    g_commandQueue->Signal(g_renderFence.Get(), 999999);
    g_renderFence->SetEventOnCompletion(999999, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
}

void Cleanup() {
    WaitForGpuIdle();
    g_broadcaster.Stop();
    g_wasapiCapture.Stop();
    g_audioRing.Close();
    g_cameraCapture.Shutdown();

    if (g_pCbvData) g_constantBuffer->Unmap(0, nullptr);
    if (g_pCameraUploadData) g_cameraUploadBuffer->Unmap(0, nullptr);
    if (g_sharedTextureHandle) CloseHandle(g_sharedTextureHandle);
    if (g_sharedFenceHandle) CloseHandle(g_sharedFenceHandle);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN:
            if (wParam == VK_F5 && !g_useCamera) {
                LoadShaderPipeline(g_loadedShaderPath);
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
