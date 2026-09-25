// --- DirectPort.cpp ---
// Unified Industrial DirectPort D3D12 Utility.
// Linear Execution Graph with Single-Window Solution & System Tray Service.
// Features:
//  - Linear execution graph: Input Acquire -> Composition -> Present & Sync -> Audio Pump
//  - Windows System Tray (notification area) integration with custom dark Acrylic context menu
//  - Full physical webcam enumeration & capture (Media Foundation, zero CPU copy)
//  - Raw Badass 256-Camera D3D12 Multiplexer Blueprint (from DirectPort-Legacy)
//  - Dynamic N x M grid layout: cols = ceil(sqrt(count)), rows = ceil(count/cols)
//  - Multiplexer produces composited grid as shared stream (DirectPort_Tex_Multiplexer)
//  - Non-blocking UDP discovery beacon (3-second cadence, loopback zero-prompt default)
//  - GPU hardware crossbar fence wait (ID3D12CommandQueue::Wait, ~170ns latency)
//  - Waitable swapchain pump; camera and audio are callback/event driven
//  - Embedded HLSL fallback shaders (never fails to render, no black screens)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wrl.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <cmath>

#include "../../sdk/directport.h"
#include "../../sdk/DirectPort_Discovery.h"
#include "../../sdk/DirectPort_Audio.h"
#include "../../sdk/DirectPort_Camera.h"
#include "Menu.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "Synchronization.lib")

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

// --- App Message & Menu Command IDs ---
#define WM_APP_TRAY_MSG        (WM_APP + 1)
#define IDM_MODE_SHADER        1001
#define IDM_MODE_CAMERA        1002
#define IDM_MODE_CONSUMER      1003
#define IDM_MODE_MULTIPLEXER   1004
#define IDM_RELOAD_SHADER      1005
#define IDM_TOGGLE_AUDIO       1006
#define IDM_CYCLE_CAMERA       1007
#define IDM_SHOW_HIDE_WINDOW   1008
#define IDM_EXIT               1009
#define IDM_CAMERA_SELECT_BASE 2000

enum DirectPortAppMode {
    MODE_PRODUCER_SHADER,
    MODE_PRODUCER_CAMERA,
    MODE_CONSUMER,
    MODE_MULTIPLEXER
};

static const UINT kFrameCount = 2;
static const UINT kDefaultWidth = 1920;
static const UINT kDefaultHeight = 1080;
static const int  MAX_MUX_PRODUCERS = 256;

// Shared Manifest Struct
struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

struct ShaderConstants {
    float u_resolution[4]; // xy = res, z = aspect, w = mode
    float u_time[4];       // x = elapsed, y = delta, z = frameIndex, w = status
    float u_mouse[4];      // xy = pos, zw = 0
};

// --- Embedded HLSL Fallback Shaders ---
static const char* g_defaultShaderHLSL = R"(
cbuffer Constants : register(b0) {
    float4 u_resolution; // xy = res, z = aspect, w = mode
    float4 u_time;       // x = elapsed, y = delta, z = frameIndex, w = status
    float4 u_mouse;      // xy = pos, zw = 0
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

PSInput VSMain(uint id : SV_VertexID) {
    PSInput o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
    o.uv = uv;
    return o;
}

float4 PSBlit(PSInput i) : SV_TARGET {
    return g_texture.Sample(g_sampler, i.uv);
}

float4 PSPlasma(PSInput i) : SV_TARGET {
    float2 uv = i.uv;
    float t = u_time.x;
    float v1 = sin(uv.x * 10.0 + t);
    float v2 = sin(uv.y * 10.0 + t * 1.2);
    float v3 = sin((uv.x + uv.y) * 8.0 + t * 0.8);
    float cx = uv.x + 0.5 * sin(t * 0.33);
    float cy = uv.y + 0.5 * cos(t * 0.5);
    float v4 = sin(sqrt(cx * cx + cy * cy + 1.0) * 12.0 + t);
    float val = v1 + v2 + v3 + v4;
    float r = sin(val * 3.14159) * 0.5 + 0.5;
    float g = sin(val * 3.14159 + 2.094) * 0.5 + 0.5;
    float b = sin(val * 3.14159 + 4.188) * 0.5 + 0.5;
    float vig = uv.x * (1.0 - uv.x) * uv.y * (1.0 - uv.y) * 16.0;
    vig = saturate(pow(vig, 0.25));
    float scan = 0.95 + 0.05 * sin(uv.y * u_resolution.y * 3.14159);
    return float4(float3(r * 0.85 + 0.15, g * 0.3, b * 0.4) * vig * scan, 1.0);
}

float4 PSBlueprint(PSInput i) : SV_TARGET {
    float2 uv = i.uv;
    float t = u_time.x;
    float2 grid = abs(frac(uv * 16.0 - 0.5) - 0.5) / max(fwidth(uv * 16.0), float2(0.0001, 0.0001));
    float lineFactor = 1.0 - min(min(grid.x, grid.y), 1.0);
    float2 quad = abs(frac(uv * 2.0 - 0.5) - 0.5) / max(fwidth(uv * 2.0), float2(0.0001, 0.0001));
    float quadLine = 1.0 - min(min(quad.x, quad.y), 1.0);
    float3 bg = float3(0.04, 0.05, 0.08);
    float3 minorGrid = float3(0.08, 0.14, 0.22) * lineFactor;
    float3 majorGrid = float3(0.78, 0.06, 0.18) * quadLine;
    float scan = 0.96 + 0.04 * sin(uv.y * 300.0 + t * 4.0);
    return float4((bg + minorGrid + majorGrid) * scan, 1.0);
}
)";

// --- Subsystem State ---
static HWND                           g_hwnd = nullptr;
static HINSTANCE                      g_instance = nullptr;
static HICON                          g_hIcon = nullptr;
static DirectPortAppMode              g_mode = MODE_PRODUCER_SHADER;
static int                            g_cameraDeviceIndex = 0;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_allocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_producerSharedRtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_producerSharedSrvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_pipelineStateBlit;
static ComPtr<ID3D12PipelineState>    g_pipelineStatePlasma;
static ComPtr<ID3D12PipelineState>    g_pipelineStateBlueprint;
static ComPtr<ID3D12PipelineState>    g_pipelineStateCustom;
static ComPtr<ID3D12Resource>         g_constantBuffer;
static ShaderConstants*               g_pCbvData = nullptr;
static UINT                           g_rtvDescriptorSize = 0;
static UINT                           g_srvDescriptorSize = 0;
static UINT                           g_frameIndex = 0;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_fenceValues[kFrameCount] = {};
static HANDLE                         g_fenceEvent = nullptr;

// --- Producer Resources ---
static ComPtr<ID3D12Resource>         g_producerSharedTexture;
static ComPtr<ID3D12Fence>            g_producerSharedFence;
static UINT64                         g_producerSharedFrameValue = 0;
static HANDLE                         g_producerSharedTexHandle = nullptr;
static HANDLE                         g_producerSharedFenceHandle = nullptr;
static HANDLE                         g_hProducerManifest = nullptr;
static BroadcastManifest*             g_pProducerManifestView = nullptr;
static std::wstring                   g_streamName = L"DirectPort_Main";
static std::wstring                   g_texHandleName = L"DirectPort_Tex_Main";
static std::wstring                   g_fenceHandleName = L"DirectPort_Fence_Main";
static std::wstring                   g_audioBufferName = L"DirectPort_Audio_Main";
static std::string                    g_shaderPath = "shaders/plasma.hlsl";
static auto                           g_producerStartTime = std::chrono::steady_clock::now();

// --- Camera Staging Resources ---
static ComPtr<ID3D12Resource>         g_cameraTexture;
static ComPtr<ID3D12DescriptorHeap>   g_cameraSrvHeap;
// One upload buffer per frame in flight: the CPU never overwrites a buffer
// the GPU may still be copying from.
static ComPtr<ID3D12Resource>         g_cameraUploadBuffer[kFrameCount];
static UINT8*                         g_pCameraUploadData[kFrameCount] = {};
static UINT64                         g_cameraFrameSerial = 0;
static UINT                           g_cameraUploadSize = 0;
static UINT                           g_cameraRowPitch = 0;

// --- Consumer Slot ---
struct ConsumerSlot {
    bool                   active = false;
    std::string            streamName;
    std::wstring           texHandleName;
    std::wstring           fenceHandleName;
    std::wstring           audioBufferName;
    ComPtr<ID3D12Resource> sharedTexture;
    ComPtr<ID3D12Fence>    sharedFence;
    HANDLE                 hSharedTex = nullptr;
    HANDLE                 hSharedFence = nullptr;
    UINT64                 lastFrame = 0;
    bool                   hasAudio = false;
};
static ConsumerSlot                   g_consumerSlot;

// --- Multiplexer (Raw Badass 256-Slot Blueprint) ---
struct MuxProducerSlot {
    bool                   isConnected = false;
    DWORD                  producerPid = 0;
    std::wstring           streamName;
    HANDLE                 hManifest = nullptr;
    BroadcastManifest*     pManifestView = nullptr;
    ComPtr<ID3D12Resource> sharedTexture;
    ComPtr<ID3D12Fence>    sharedFence;
    UINT64                 lastSeenFrame = 0;
    ComPtr<ID3D12Resource> privateTexture;
    UINT                   srvDescriptorIndex = 0;
    HANDLE                 hSharedTex = nullptr;
    HANDLE                 hSharedFence = nullptr;
};
static MuxProducerSlot                g_muxProducers[MAX_MUX_PRODUCERS];
static ComPtr<ID3D12DescriptorHeap>   g_muxSrvHeap;
static ComPtr<ID3D12Resource>         g_muxCompositeTexture;
static ComPtr<ID3D12DescriptorHeap>   g_muxCompositeRtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_muxCompositeSrvHeap;
static ComPtr<ID3D12Resource>         g_muxSharedOutTexture;
static ComPtr<ID3D12Fence>            g_muxSharedOutFence;
static UINT64                         g_muxSharedOutFrameValue = 0;
static HANDLE                         g_hMuxManifestOut = nullptr;
static BroadcastManifest*             g_pMuxManifestViewOut = nullptr;
static HANDLE                         g_muxSharedOutTexHandle = nullptr;
static HANDLE                         g_muxSharedOutFenceHandle = nullptr;
static std::wstring                   g_muxTexName;
static std::wstring                   g_muxFenceName;

// --- Networking & Audio Services ---
static DirectPortDiscoveryBroadcaster g_broadcaster;
static DirectPortDiscoveryListener    g_listener;
static std::vector<DirectPortDiscoveryListener::DiscoveredStream> g_discoveredStreams;
static DirectPortAudioRingProducer    g_audioRingProducer;
static DirectPortAudioRingConsumer    g_audioRingConsumer;
static DirectPortWASAPICapture        g_wasapiCapture;
static DirectPortCameraCapture        g_cameraCapture;
static bool                           g_enableAudio = true;
static bool                           g_lanMode = false;
static auto                           g_lastDiscoveryCheck = std::chrono::steady_clock::now() - std::chrono::seconds(5);

// --- Audio Playback Thread ---
static HANDLE                         g_hAudioPlayThread = nullptr;
static HANDLE                         g_hAudioPlayStopEvent = nullptr;
static bool                           g_audioPlaying = false;

// --- Forward Declarations ---
void ApplyMicaWindowAttributes(HWND hwnd);
void ManageTrayIcon(HWND hwnd, bool add);
void ShowContextMenu(HWND hwnd, POINT pt);
bool InitD3D12(HWND hwnd);
bool InitPipelines();
bool LoadProducerShader(const std::string& path);
bool InitProducerSharedResources();
void TeardownProducerResources();
bool InitCameraResources();
void TeardownCameraResources();
void TeardownConsumerResources();
bool InitMuxResources();
void TeardownMuxResources();
void Mux_FindAndConnectProducers();
void Mux_DisconnectProducer(int i);
void SwitchMode(DirectPortAppMode newMode);
void SwitchCamera(int deviceIndex);
void ToggleAudio();
void StepGraph_Discovery();
void StepGraph_InputAcquire();
void StepGraph_Composition(float totalW, float totalH);
void StepGraph_PresentAndSync();
void MoveToNextFrame();
void WaitForGpu();
void ResizeSwapChain(UINT width, UINT height);
void Cleanup();
void StartAudioPlayback();
void StopAudioPlayback();
DWORD WINAPI AudioPlaybackThreadProc(LPVOID);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    g_instance = hInstance;

    // Initialize COM on main thread for Media Foundation & WASAPI
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool modeSetByCmd = false;

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--produce") == 0 || _wcsicmp(argv[i], L"-p") == 0 || _wcsicmp(argv[i], L"--shader") == 0) {
            g_mode = MODE_PRODUCER_SHADER;
            modeSetByCmd = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                char buf[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, buf, sizeof(buf), NULL, NULL);
                g_shaderPath = buf;
            }
        } else if (_wcsicmp(argv[i], L"--camera") == 0 || _wcsicmp(argv[i], L"-c") == 0) {
            g_mode = MODE_PRODUCER_CAMERA;
            modeSetByCmd = true;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                g_cameraDeviceIndex = _wtoi(argv[++i]);
            }
        } else if (_wcsicmp(argv[i], L"--device") == 0 && i + 1 < argc) {
            g_cameraDeviceIndex = _wtoi(argv[++i]);
        } else if (_wcsicmp(argv[i], L"--consume") == 0 || _wcsicmp(argv[i], L"-s") == 0) {
            g_mode = MODE_CONSUMER;
            modeSetByCmd = true;
        } else if (_wcsicmp(argv[i], L"--mux") == 0 || _wcsicmp(argv[i], L"-m") == 0) {
            g_mode = MODE_MULTIPLEXER;
            modeSetByCmd = true;
        } else if (_wcsicmp(argv[i], L"--no-audio") == 0) {
            g_enableAudio = false;
        } else if (_wcsicmp(argv[i], L"--lan") == 0) {
            g_lanMode = true;
        }
    }
    LocalFree(argv);

    // If no mode explicitly set, check if a webcam is available. If so, start camera feed!
    if (!modeSetByCmd) {
        auto cams = DirectPortCameraCapture::EnumerateCameras();
        if (!cams.empty()) {
            g_mode = MODE_PRODUCER_CAMERA;
        } else {
            g_mode = MODE_PRODUCER_SHADER;
        }
    }

    g_hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    if (!g_hIcon) g_hIcon = LoadIconW(nullptr, IDI_APPLICATION);

    const WCHAR szClass[] = L"DirectPortClass";
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = szClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = g_hIcon;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, szClass, L"DirectPort // D3D12 Hardware Crossbar", 
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    ApplyMicaWindowAttributes(g_hwnd);
    ManageTrayIcon(g_hwnd, true);

    if (!InitD3D12(g_hwnd)) return 1;
    if (!InitPipelines()) return 1;

    g_listener.Start(!g_lanMode);
    SwitchMode(g_mode);

    ShowWindow(g_hwnd, nCmdShow);

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

        if (!running) break;

        // Stage 0: Non-blocking discovery cadence (every 2-3 seconds)
        StepGraph_Discovery();

        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
            RECT clientRc;
            GetClientRect(g_hwnd, &clientRc);
            float totalW = (std::max)(1.0f, (float)(clientRc.right - clientRc.left));
            float totalH = (std::max)(1.0f, (float)(clientRc.bottom - clientRc.top));

            g_allocators[g_frameIndex]->Reset();
            g_commandList->Reset(g_allocators[g_frameIndex].Get(), nullptr);

            StepGraph_InputAcquire();
            StepGraph_Composition(totalW, totalH);
            StepGraph_PresentAndSync();
        }
    }

    Cleanup();
    CoUninitialize();
    return 0;
}

void ApplyMicaWindowAttributes(HWND hwnd) {
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    COLORREF captionColor = 0xFFFFFFFE;
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

    COLORREF textColor = 0x00FFFFFF;
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));

    int backdrop = 4; // Mica Alt
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
        backdrop = 2; // Mica fallback
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    }
}

void ManageTrayIcon(HWND hwnd, bool add) {
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = 1;
    if (add) {
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY_MSG;
        nid.hIcon = g_hIcon;
        wcscpy_s(nid.szTip, L"DirectPort // D3D12 Hardware Crossbar");
        Shell_NotifyIconW(NIM_ADD, &nid);
    } else {
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
}

void ShowContextMenu(HWND hwnd, POINT pt) {
    auto* menu = new PopupMenu(hwnd, g_instance);
    menu->AddItem(L"Producer: procedural shader  (1)", IDM_MODE_SHADER, g_mode == MODE_PRODUCER_SHADER);
    menu->AddItem(L"Producer: live camera  (2)", IDM_MODE_CAMERA, g_mode == MODE_PRODUCER_CAMERA);
    menu->AddItem(L"Consumer: auto-listen  (3)", IDM_MODE_CONSUMER, g_mode == MODE_CONSUMER);
    menu->AddItem(L"Multiplexer: grid of all streams  (4)", IDM_MODE_MULTIPLEXER, g_mode == MODE_MULTIPLEXER);
    menu->AddSeparator();

    auto cams = DirectPortCameraCapture::EnumerateCameras();
    if (!cams.empty()) {
        PopupMenu* cameras = menu->AddSubMenu(L"Camera");
        for (const auto& c : cams) {
            const bool active = (g_mode == MODE_PRODUCER_CAMERA && g_cameraDeviceIndex == c.index);
            cameras->AddItem(c.friendlyName, IDM_CAMERA_SELECT_BASE + c.index, active);
        }
        cameras->AddSeparator();
        cameras->AddItem(L"Next camera  (C)", IDM_CYCLE_CAMERA);
    }

    menu->AddItem(L"Reload HLSL shader  (5)", IDM_RELOAD_SHADER);
    menu->AddItem(L"Audio  (M)", IDM_TOGGLE_AUDIO, g_enableAudio);
    menu->AddSeparator();
    menu->AddItem(IsWindowVisible(hwnd) ? L"Hide window" : L"Show window", IDM_SHOW_HIDE_WINDOW);
    menu->AddItem(L"Exit", IDM_EXIT);
    menu->ShowAt(pt);
}

bool InitD3D12(HWND hwnd) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) return false;

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&g_commandQueue)))) return false;

    RECT rc;
    GetClientRect(hwnd, &rc);
    UINT width = (UINT)(std::max)(1, (int)(rc.right - rc.left));
    UINT height = (UINT)(std::max)(1, (int)(rc.bottom - rc.top));

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = width;
    scDesc.Height = height;
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = 16;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC prodRtvDesc = {};
    prodRtvDesc.NumDescriptors = 1;
    prodRtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&prodRtvDesc, IID_PPV_ARGS(&g_producerSharedRtvHeap));

    D3D12_DESCRIPTOR_HEAP_DESC prodSrvDesc = {};
    prodSrvDesc.NumDescriptors = 1;
    prodSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    prodSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&prodSrvDesc, IID_PPV_ARGS(&g_producerSharedSrvHeap));

    // Camera SRV Heap
    D3D12_DESCRIPTOR_HEAP_DESC camSrvDesc = {};
    camSrvDesc.NumDescriptors = 1;
    camSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    camSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&camSrvDesc, IID_PPV_ARGS(&g_cameraSrvHeap));

    // Multiplexer SRV Heap (256 slots)
    D3D12_DESCRIPTOR_HEAP_DESC muxSrvDesc = {};
    muxSrvDesc.NumDescriptors = MAX_MUX_PRODUCERS;
    muxSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    muxSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&muxSrvDesc, IID_PPV_ARGS(&g_muxSrvHeap));

    // Multiplexer Composite RTV & SRV Heaps
    D3D12_DESCRIPTOR_HEAP_DESC muxCompRtvDesc = {};
    muxCompRtvDesc.NumDescriptors = 1;
    muxCompRtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&muxCompRtvDesc, IID_PPV_ARGS(&g_muxCompositeRtvHeap));

    D3D12_DESCRIPTOR_HEAP_DESC muxCompSrvDesc = {};
    muxCompSrvDesc.NumDescriptors = 1;
    muxCompSrvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    muxCompSrvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&muxCompSrvDesc, IID_PPV_ARGS(&g_muxCompositeSrvHeap));

    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // The first frame's fence value must be non-zero, otherwise the first
    // MoveToNextFrame() records 0 for a buffer and its allocator is later
    // reset while the GPU may still be using it.
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    g_fenceValues[g_frameIndex] = 1;

    D3D12_HEAP_PROPERTIES uploadHeap = { D3D12_HEAP_TYPE_UPLOAD };
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

    return true;
}

bool InitPipelines() {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rootParams[2] = {};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &range;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob, errBlob;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errBlob))) return false;
    if (FAILED(g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature)))) return false;

    // Compile embedded shaders
    ComPtr<ID3DBlob> vsBlob, psBlitBlob, psPlasmaBlob, psBlueprintBlob;
    D3DCompile(g_defaultShaderHLSL, strlen(g_defaultShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_defaultShaderHLSL, strlen(g_defaultShaderHLSL), nullptr, nullptr, nullptr, "PSBlit", "ps_5_0", 0, 0, &psBlitBlob, nullptr);
    D3DCompile(g_defaultShaderHLSL, strlen(g_defaultShaderHLSL), nullptr, nullptr, nullptr, "PSPlasma", "ps_5_0", 0, 0, &psPlasmaBlob, nullptr);
    D3DCompile(g_defaultShaderHLSL, strlen(g_defaultShaderHLSL), nullptr, nullptr, nullptr, "PSBlueprint", "ps_5_0", 0, 0, &psBlueprintBlob, nullptr);

    if (!vsBlob || !psBlitBlob || !psPlasmaBlob || !psBlueprintBlob) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    // Blit PSO
    psoDesc.PS = { psBlitBlob->GetBufferPointer(), psBlitBlob->GetBufferSize() };
    if (FAILED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineStateBlit)))) return false;

    // Plasma PSO
    psoDesc.PS = { psPlasmaBlob->GetBufferPointer(), psPlasmaBlob->GetBufferSize() };
    if (FAILED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineStatePlasma)))) return false;

    // Blueprint PSO
    psoDesc.PS = { psBlueprintBlob->GetBufferPointer(), psBlueprintBlob->GetBufferSize() };
    if (FAILED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineStateBlueprint)))) return false;

    LoadProducerShader(g_shaderPath);
    return true;
}

bool LoadProducerShader(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return false;
    std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    HRESULT hr = D3DCompile(source.c_str(), source.length(), path.c_str(), nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) return false;

    hr = D3DCompile(source.c_str(), source.length(), path.c_str(), nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    if (FAILED(hr) || !vsBlob) {
        D3DCompile(g_defaultShaderHLSL, strlen(g_defaultShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    }

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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    return SUCCEEDED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineStateCustom)));
}

bool InitProducerSharedResources() {
    TeardownProducerResources();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kDefaultWidth;
    texDesc.Height = kDefaultHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (FAILED(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_SHARED, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, &clearVal, IID_PPV_ARGS(&g_producerSharedTexture)))) return false;

    g_device->CreateRenderTargetView(g_producerSharedTexture.Get(), nullptr, g_producerSharedRtvHeap->GetCPUDescriptorHandleForHeapStart());
    g_device->CreateShaderResourceView(g_producerSharedTexture.Get(), nullptr, g_producerSharedSrvHeap->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_producerSharedFence)))) return false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

    DWORD pid = GetCurrentProcessId();
    // Local\ (session) names: Global\ needs SeCreateGlobalPrivilege, which
    // standard users do not have, so CreateSharedHandle would fail for them.
    g_texHandleName = L"Local\\DirectPortTexture_" + std::to_wstring(pid);
    g_fenceHandleName = L"Local\\DirectPortFence_" + std::to_wstring(pid);

    g_device->CreateSharedHandle(g_producerSharedTexture.Get(), &sa, GENERIC_ALL, g_texHandleName.c_str(), &g_producerSharedTexHandle);
    g_device->CreateSharedHandle(g_producerSharedFence.Get(), &sa, GENERIC_ALL, g_fenceHandleName.c_str(), &g_producerSharedFenceHandle);

    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
    g_hProducerManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());

    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);

    if (g_hProducerManifest) {
        g_pProducerManifestView = (BroadcastManifest*)MapViewOfFile(g_hProducerManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
        if (g_pProducerManifestView) {
            ZeroMemory(g_pProducerManifestView, sizeof(BroadcastManifest));
            g_pProducerManifestView->width = kDefaultWidth;
            g_pProducerManifestView->height = kDefaultHeight;
            g_pProducerManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
            g_pProducerManifestView->adapterLuid = g_device->GetAdapterLuid();
            wcscpy_s(g_pProducerManifestView->textureName, g_texHandleName.c_str());
            wcscpy_s(g_pProducerManifestView->fenceName, g_fenceHandleName.c_str());
        }
    }

    if (g_enableAudio) {
        if (g_audioRingProducer.Initialize(g_audioBufferName.c_str(), 48000, 2)) {
            g_wasapiCapture.Start(&g_audioRingProducer);
        }
    }

    g_broadcaster.Start();
    g_producerStartTime = std::chrono::steady_clock::now();
    return true;
}

void TeardownProducerResources() {
    g_broadcaster.Stop();
    g_wasapiCapture.Stop();
    g_audioRingProducer.Close();

    if (g_pProducerManifestView) { UnmapViewOfFile(g_pProducerManifestView); g_pProducerManifestView = nullptr; }
    if (g_hProducerManifest) { CloseHandle(g_hProducerManifest); g_hProducerManifest = nullptr; }
    if (g_producerSharedTexHandle) { CloseHandle(g_producerSharedTexHandle); g_producerSharedTexHandle = nullptr; }
    if (g_producerSharedFenceHandle) { CloseHandle(g_producerSharedFenceHandle); g_producerSharedFenceHandle = nullptr; }
    g_producerSharedTexture.Reset();
    g_producerSharedFence.Reset();
}

bool InitCameraResources() {
    TeardownCameraResources();

    if (!g_cameraCapture.Initialize(g_cameraDeviceIndex)) return false;

    UINT camW = g_cameraCapture.GetWidth();
    UINT camH = g_cameraCapture.GetHeight();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = camW;
    texDesc.Height = camH;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defHeap = { D3D12_HEAP_TYPE_DEFAULT };
    if (FAILED(g_device->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_cameraTexture)))) return false;

    g_device->CreateShaderResourceView(g_cameraTexture.Get(), nullptr, g_cameraSrvHeap->GetCPUDescriptorHandleForHeapStart());

    // Aligned upload buffer (256-byte pitch requirement in D3D12)
    g_cameraRowPitch = (camW * 4 + 255) & ~255;
    g_cameraUploadSize = g_cameraRowPitch * camH;

    D3D12_RESOURCE_DESC upDesc = {};
    upDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upDesc.Width = g_cameraUploadSize;
    upDesc.Height = 1;
    upDesc.DepthOrArraySize = 1;
    upDesc.MipLevels = 1;
    upDesc.SampleDesc.Count = 1;
    upDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES upHeap = { D3D12_HEAP_TYPE_UPLOAD };
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (FAILED(g_device->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &upDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cameraUploadBuffer[i])))) return false;
        g_cameraUploadBuffer[i]->Map(0, nullptr, reinterpret_cast<void**>(&g_pCameraUploadData[i]));
    }
    g_cameraFrameSerial = 0;
    return true;
}

void TeardownCameraResources() {
    g_cameraCapture.Shutdown();
    for (UINT i = 0; i < kFrameCount; ++i) {
        if (g_pCameraUploadData[i]) {
            g_cameraUploadBuffer[i]->Unmap(0, nullptr);
            g_pCameraUploadData[i] = nullptr;
        }
        g_cameraUploadBuffer[i].Reset();
    }
    g_cameraTexture.Reset();
    g_cameraUploadSize = 0;
    g_cameraRowPitch = 0;
}

void TeardownConsumerResources() {
    StopAudioPlayback();
    g_audioRingConsumer.Close();
    if (g_consumerSlot.hSharedTex) { CloseHandle(g_consumerSlot.hSharedTex); g_consumerSlot.hSharedTex = nullptr; }
    if (g_consumerSlot.hSharedFence) { CloseHandle(g_consumerSlot.hSharedFence); g_consumerSlot.hSharedFence = nullptr; }
    g_consumerSlot.sharedTexture.Reset();
    g_consumerSlot.sharedFence.Reset();
    g_consumerSlot.active = false;
}

bool InitMuxResources() {
    TeardownMuxResources();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.Width = kDefaultWidth;
    texDesc.Height = kDefaultHeight;
    texDesc.MipLevels = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES defaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (FAILED(g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc, 
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearVal, IID_PPV_ARGS(&g_muxCompositeTexture)))) return false;

    g_device->CreateRenderTargetView(g_muxCompositeTexture.Get(), nullptr, g_muxCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart());
    g_device->CreateShaderResourceView(g_muxCompositeTexture.Get(), nullptr, g_muxCompositeSrvHeap->GetCPUDescriptorHandleForHeapStart());

    texDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (FAILED(g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, 
        D3D12_RESOURCE_STATE_COMMON, &clearVal, IID_PPV_ARGS(&g_muxSharedOutTexture)))) return false;

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL);
    sa.lpSecurityDescriptor = sd;

    DWORD pid = GetCurrentProcessId();
    g_muxTexName = L"Local\\DirectPortTexture_Multiplexer_" + std::to_wstring(pid);
    g_muxFenceName = L"Local\\DirectPortFence_Multiplexer_" + std::to_wstring(pid);
    const std::wstring& textureName = g_muxTexName;
    const std::wstring& fenceName = g_muxFenceName;
    g_device->CreateSharedHandle(g_muxSharedOutTexture.Get(), &sa, GENERIC_ALL, textureName.c_str(), &g_muxSharedOutTexHandle);
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_muxSharedOutFence));
    g_device->CreateSharedHandle(g_muxSharedOutFence.Get(), &sa, GENERIC_ALL, fenceName.c_str(), &g_muxSharedOutFenceHandle);

    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(pid);
    g_hMuxManifestOut = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    if (sd) LocalFree(sd);

    if (g_hMuxManifestOut) {
        g_pMuxManifestViewOut = (BroadcastManifest*)MapViewOfFile(g_hMuxManifestOut, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
        if (g_pMuxManifestViewOut) {
            ZeroMemory(g_pMuxManifestViewOut, sizeof(BroadcastManifest));
            g_pMuxManifestViewOut->width = kDefaultWidth;
            g_pMuxManifestViewOut->height = kDefaultHeight;
            g_pMuxManifestViewOut->format = DXGI_FORMAT_B8G8R8A8_UNORM;
            g_pMuxManifestViewOut->adapterLuid = g_device->GetAdapterLuid();
            wcscpy_s(g_pMuxManifestViewOut->textureName, textureName.c_str());
            wcscpy_s(g_pMuxManifestViewOut->fenceName, fenceName.c_str());
        }
    }

    g_broadcaster.Start();
    return true;
}

void TeardownMuxResources() {
    for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) Mux_DisconnectProducer(i);

    if (g_pMuxManifestViewOut) { UnmapViewOfFile(g_pMuxManifestViewOut); g_pMuxManifestViewOut = nullptr; }
    if (g_hMuxManifestOut) { CloseHandle(g_hMuxManifestOut); g_hMuxManifestOut = nullptr; }
    if (g_muxSharedOutTexHandle) { CloseHandle(g_muxSharedOutTexHandle); g_muxSharedOutTexHandle = nullptr; }
    if (g_muxSharedOutFenceHandle) { CloseHandle(g_muxSharedOutFenceHandle); g_muxSharedOutFenceHandle = nullptr; }
    g_muxSharedOutTexture.Reset();
    g_muxSharedOutFence.Reset();
    g_muxCompositeTexture.Reset();
}

void Mux_DisconnectProducer(int i) {
    auto& p = g_muxProducers[i];
    if (!p.isConnected) return;
    if (p.pManifestView) UnmapViewOfFile(p.pManifestView);
    if (p.hManifest) CloseHandle(p.hManifest);
    if (p.hSharedTex) CloseHandle(p.hSharedTex);
    if (p.hSharedFence) CloseHandle(p.hSharedFence);
    p = {};
}

void Mux_FindAndConnectProducers() {
    // 1. Clean up dead processes
    for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
        if (!g_muxProducers[i].isConnected) continue;
        if (g_muxProducers[i].producerPid != 0) {
            HANDLE hProcess = OpenProcess(SYNCHRONIZE, FALSE, g_muxProducers[i].producerPid);
            if (hProcess == NULL || WaitForSingleObject(hProcess, 0) != WAIT_TIMEOUT) {
                Mux_DisconnectProducer(i);
            }
            if (hProcess) CloseHandle(hProcess);
        }
    }

    // 2. Discover via Toolhelp32 process snapshot looking for manifests
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe32 = { sizeof(pe32) };
        DWORD selfPid = GetCurrentProcessId();

        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == selfPid) continue;

                bool alreadyConnected = false;
                for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
                    if (g_muxProducers[i].isConnected && g_muxProducers[i].producerPid == pe32.th32ProcessID) {
                        alreadyConnected = true;
                        break;
                    }
                }
                if (alreadyConnected) continue;

                int availableSlot = -1;
                for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
                    if (!g_muxProducers[i].isConnected) { availableSlot = i; break; }
                }
                if (availableSlot == -1) break;

                const std::vector<std::wstring> prefixes = { L"DirectPort_Producer_Manifest_", L"D3D12_Producer_Manifest_" };
                HANDLE hManifest = nullptr;
                for (const auto& prefix : prefixes) {
                    std::wstring manifestName = prefix + std::to_wstring(pe32.th32ProcessID);
                    hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                    if (hManifest) break;
                }
                if (!hManifest) continue;

                BroadcastManifest* pManifestView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                if (!pManifestView) { CloseHandle(hManifest); continue; }

                auto& producer = g_muxProducers[availableSlot];
                HANDLE hTexture = nullptr, hFence = nullptr;
                g_device->OpenSharedHandleByName(pManifestView->textureName, GENERIC_ALL, &hTexture);
                g_device->OpenSharedHandleByName(pManifestView->fenceName, GENERIC_ALL, &hFence);

                if (hTexture && hFence) {
                    g_device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&producer.sharedTexture));
                    g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&producer.sharedFence));
                    producer.hSharedTex = hTexture;
                    producer.hSharedFence = hFence;
                } else {
                    if (hTexture) CloseHandle(hTexture);
                    if (hFence) CloseHandle(hFence);
                }

                if (producer.sharedTexture && producer.sharedFence) {
                    producer.isConnected = true;
                    producer.producerPid = pe32.th32ProcessID;
                    producer.hManifest = hManifest;
                    producer.pManifestView = pManifestView;
                    producer.lastSeenFrame = (pManifestView->frameValue > 0) ? (pManifestView->frameValue - 1) : 0;
                    producer.srvDescriptorIndex = availableSlot;

                    D3D12_RESOURCE_DESC desc = producer.sharedTexture->GetDesc();
                    desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

                    D3D12_HEAP_PROPERTIES defaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT };
                    g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &desc, 
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&producer.privateTexture));

                    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = g_muxSrvHeap->GetCPUDescriptorHandleForHeapStart();
                    srvHandle.ptr += (UINT64)producer.srvDescriptorIndex * g_srvDescriptorSize;
                    g_device->CreateShaderResourceView(producer.privateTexture.Get(), nullptr, srvHandle);
                } else {
                    UnmapViewOfFile(pManifestView);
                    CloseHandle(hManifest);
                }
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }

    // 3. Also connect to UDP-discovered streams
    for (const auto& d : g_discoveredStreams) {
        bool alreadyConnected = false;
        for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
            if (g_muxProducers[i].isConnected && g_muxProducers[i].streamName == d.textureHandleName) {
                alreadyConnected = true;
                break;
            }
        }
        if (alreadyConnected) continue;

        int availableSlot = -1;
        for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
            if (!g_muxProducers[i].isConnected) { availableSlot = i; break; }
        }
        if (availableSlot == -1) break;

        HANDLE hTexture = nullptr, hFence = nullptr;
        if (SUCCEEDED(g_device->OpenSharedHandleByName(d.textureHandleName.c_str(), GENERIC_ALL, &hTexture)) &&
            SUCCEEDED(g_device->OpenSharedHandleByName(d.fenceHandleName.c_str(), GENERIC_ALL, &hFence))) {
            auto& producer = g_muxProducers[availableSlot];
            g_device->OpenSharedHandle(hTexture, IID_PPV_ARGS(&producer.sharedTexture));
            g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&producer.sharedFence));
            producer.hSharedTex = hTexture;
            producer.hSharedFence = hFence;
            producer.streamName = d.textureHandleName;
            producer.isConnected = true;
            producer.producerPid = 0;
            producer.srvDescriptorIndex = availableSlot;

            D3D12_RESOURCE_DESC desc = producer.sharedTexture->GetDesc();
            desc.Flags &= ~D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_HEAP_PROPERTIES defaultHeapProps = { D3D12_HEAP_TYPE_DEFAULT };
            g_device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &desc, 
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&producer.privateTexture));

            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = g_muxSrvHeap->GetCPUDescriptorHandleForHeapStart();
            srvHandle.ptr += (UINT64)producer.srvDescriptorIndex * g_srvDescriptorSize;
            g_device->CreateShaderResourceView(producer.privateTexture.Get(), nullptr, srvHandle);
        } else {
            if (hTexture) CloseHandle(hTexture);
            if (hFence) CloseHandle(hFence);
        }
    }
}

void SwitchCamera(int deviceIndex) {
    g_cameraDeviceIndex = deviceIndex;
    SwitchMode(MODE_PRODUCER_CAMERA);
}

void SwitchMode(DirectPortAppMode newMode) {
    // Resources below may still be referenced by in-flight command lists.
    WaitForGpu();
    TeardownProducerResources();
    TeardownCameraResources();
    TeardownConsumerResources();
    TeardownMuxResources();

    g_mode = newMode;

    switch (g_mode) {
        case MODE_PRODUCER_SHADER:
            InitProducerSharedResources();
            SetWindowTextW(g_hwnd, L"DirectPort [PRODUCER: HLSL Shader (1920x1080)] // 1: Shader | 5: Reload");
            break;

        case MODE_PRODUCER_CAMERA:
            if (!InitCameraResources()) {
                // If webcam unavailable or failed, fallback gracefully to shader
                SwitchMode(MODE_PRODUCER_SHADER);
                return;
            }
            InitProducerSharedResources();
            {
                std::wstring title = L"DirectPort [PRODUCER: " + g_cameraCapture.GetDeviceName() + 
                    L" (" + std::to_wstring(g_cameraCapture.GetWidth()) + L"x" + std::to_wstring(g_cameraCapture.GetHeight()) + 
                    L")] // 2: Cam | C: Cycle";
                SetWindowTextW(g_hwnd, title.c_str());
            }
            break;

        case MODE_CONSUMER:
            SetWindowTextW(g_hwnd, L"DirectPort [CONSUMER: Scanning 127.0.0.1:3987 & Manifests...] // 3");
            break;

        case MODE_MULTIPLEXER:
            InitMuxResources();
            SetWindowTextW(g_hwnd, L"DirectPort [MULTIPLEXER: 256-Camera Blueprint Active] // 4");
            break;
    }
}

void ToggleAudio() {
    g_enableAudio = !g_enableAudio;
    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        if (g_enableAudio) {
            if (g_audioRingProducer.Initialize(g_audioBufferName.c_str(), 48000, 2)) {
                g_wasapiCapture.Start(&g_audioRingProducer);
            }
        } else {
            g_wasapiCapture.Stop();
            g_audioRingProducer.Close();
        }
    } else if (g_mode == MODE_CONSUMER) {
        if (!g_enableAudio) StopAudioPlayback();
    }
}

// Stage 0: Non-blocking Discovery Cadence
void StepGraph_Discovery() {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - g_lastDiscoveryCheck).count() < 2) return;
    g_lastDiscoveryCheck = now;

    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        g_broadcaster.Broadcast("DirectPort_Main", kDefaultWidth, kDefaultHeight, DXGI_FORMAT_B8G8R8A8_UNORM,
            g_texHandleName.c_str(), g_fenceHandleName.c_str(), g_producerSharedFrameValue,
            g_enableAudio, 48000, 2, g_audioBufferName.c_str());
    } else if (g_mode == MODE_CONSUMER) {
        g_listener.Poll(g_discoveredStreams);

        // Also check local manifests if no UDP stream discovered yet
        if (!g_consumerSlot.active) {
            // Check discovered streams first
            if (!g_discoveredStreams.empty()) {
                const auto& d = g_discoveredStreams[0];
                HANDLE hTex = nullptr, hFence = nullptr;
                if (SUCCEEDED(g_device->OpenSharedHandleByName(d.textureHandleName.c_str(), GENERIC_ALL, &hTex)) &&
                    SUCCEEDED(g_device->OpenSharedHandleByName(d.fenceHandleName.c_str(), GENERIC_ALL, &hFence))) {
                    g_device->OpenSharedHandle(hTex, IID_PPV_ARGS(&g_consumerSlot.sharedTexture));
                    g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&g_consumerSlot.sharedFence));
                    g_consumerSlot.hSharedTex = hTex;
                    g_consumerSlot.hSharedFence = hFence;
                    g_consumerSlot.active = true;
                    g_consumerSlot.streamName = d.streamName;
                    g_consumerSlot.hasAudio = d.hasAudio;

                    g_device->CreateShaderResourceView(g_consumerSlot.sharedTexture.Get(), nullptr, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

                    if (g_enableAudio && d.hasAudio && !d.audioBufferName.empty()) {
                        if (g_audioRingConsumer.Open(d.audioBufferName.c_str())) StartAudioPlayback();
                    }

                    std::wstring title = L"DirectPort [CONSUMER: " + std::wstring(d.streamName.begin(), d.streamName.end()) + L"]" + (d.hasAudio ? L" [A/V]" : L"");
                    SetWindowTextW(g_hwnd, title.c_str());
                }
            }
        }
    } else if (g_mode == MODE_MULTIPLEXER) {
        g_listener.Poll(g_discoveredStreams);
        Mux_FindAndConnectProducers();

        // Broadcast the multiplexed grid stream out
        g_broadcaster.Broadcast("DirectPort_Multiplexer", kDefaultWidth, kDefaultHeight, DXGI_FORMAT_B8G8R8A8_UNORM,
            g_muxTexName.c_str(), g_muxFenceName.c_str(), g_muxSharedOutFrameValue,
            false, 0, 0, L"");

        int count = 0;
        for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) if (g_muxProducers[i].isConnected) count++;
        std::wstring title = L"DirectPort [MULTIPLEXER: " + std::to_wstring(count) + L" Streams Active / 256 Slot Blueprint]";
        SetWindowTextW(g_hwnd, title.c_str());
    }
}

// Stage 1: Input Acquire
void StepGraph_InputAcquire() {
    auto now = std::chrono::steady_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_producerStartTime).count();
    g_pCbvData->u_resolution[0] = (float)kDefaultWidth;
    g_pCbvData->u_resolution[1] = (float)kDefaultHeight;
    g_pCbvData->u_resolution[2] = (float)kDefaultWidth / (float)kDefaultHeight;
    g_pCbvData->u_time[0] = elapsed;
    g_pCbvData->u_time[2] = (float)g_producerSharedFrameValue;

    if (g_mode == MODE_PRODUCER_SHADER) {
        // Draw procedural shader directly to g_producerSharedTexture
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_producerSharedTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_producerSharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_commandList->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)kDefaultWidth, (float)kDefaultHeight, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)kDefaultWidth, (LONG)kDefaultHeight };
        g_commandList->RSSetViewports(1, &vp);
        g_commandList->RSSetScissorRects(1, &sc);

        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
        g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());

        if (g_pipelineStateCustom) {
            g_commandList->SetPipelineState(g_pipelineStateCustom.Get());
        } else {
            g_commandList->SetPipelineState(g_pipelineStatePlasma.Get());
        }

        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_commandList->DrawInstanced(3, 1, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &barrier);
    } else if (g_mode == MODE_PRODUCER_CAMERA && g_cameraCapture.IsActive()) {
        // Upload only when the camera delivered a new frame; never block on it.
        if (g_cameraCapture.CopyLatestFrame(g_pCameraUploadData[g_frameIndex], g_cameraRowPitch, g_cameraFrameSerial)) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_cameraTexture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            g_commandList->ResourceBarrier(1, &barrier);

            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = g_cameraTexture.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;

            src.pResource = g_cameraUploadBuffer[g_frameIndex].Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint.Width = g_cameraCapture.GetWidth();
            src.PlacedFootprint.Footprint.Height = g_cameraCapture.GetHeight();
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = g_cameraRowPitch;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

            g_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_commandList->ResourceBarrier(1, &barrier);
        }

        // Blit g_cameraTexture to g_producerSharedTexture (1080p output)
        D3D12_RESOURCE_BARRIER prodBarrier = {};
        prodBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        prodBarrier.Transition.pResource = g_producerSharedTexture.Get();
        prodBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        prodBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &prodBarrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_producerSharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_commandList->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);

        D3D12_VIEWPORT vp = { 0, 0, (float)kDefaultWidth, (float)kDefaultHeight, 0.0f, 1.0f };
        D3D12_RECT sc = { 0, 0, (LONG)kDefaultWidth, (LONG)kDefaultHeight };
        g_commandList->RSSetViewports(1, &vp);
        g_commandList->RSSetScissorRects(1, &sc);

        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
        g_commandList->SetPipelineState(g_pipelineStateBlit.Get());

        ID3D12DescriptorHeap* heaps[] = { g_cameraSrvHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, heaps);
        g_commandList->SetGraphicsRootDescriptorTable(1, g_cameraSrvHeap->GetGPUDescriptorHandleForHeapStart());

        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_commandList->DrawInstanced(3, 1, 0, 0);

        prodBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        prodBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &prodBarrier);
    } else if (g_mode == MODE_CONSUMER) {
        if (g_consumerSlot.active && g_consumerSlot.sharedFence) {
            UINT64 frame = g_consumerSlot.sharedFence->GetCompletedValue();
            if (frame > g_consumerSlot.lastFrame) {
                g_commandQueue->Wait(g_consumerSlot.sharedFence.Get(), frame);
                g_consumerSlot.lastFrame = frame;
            }
        }
    }
}

// Stage 2: Composition
void StepGraph_Composition(float totalW, float totalH) {
    if (g_mode == MODE_MULTIPLEXER) {
        // --- RAW BADASS 256-CAMERA MULTIPLEXER PIPELINE ---
        
        // 1. Ingest from active producers
        std::vector<D3D12_RESOURCE_BARRIER> preCopyBarriers;
        for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
            auto& p = g_muxProducers[i];
            if (p.isConnected && p.privateTexture) {
                UINT64 latestFrame = p.pManifestView ? p.pManifestView->frameValue : (p.sharedFence ? p.sharedFence->GetCompletedValue() : 0);
                if (latestFrame > p.lastSeenFrame) {
                    g_commandQueue->Wait(p.sharedFence.Get(), latestFrame);

                    D3D12_RESOURCE_BARRIER b = {};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = p.privateTexture.Get();
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    preCopyBarriers.push_back(b);
                }
            }
        }
        if (!preCopyBarriers.empty()) g_commandList->ResourceBarrier((UINT)preCopyBarriers.size(), preCopyBarriers.data());

        for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
            auto& p = g_muxProducers[i];
            if (p.isConnected && p.privateTexture) {
                UINT64 latestFrame = p.pManifestView ? p.pManifestView->frameValue : (p.sharedFence ? p.sharedFence->GetCompletedValue() : 0);
                if (latestFrame > p.lastSeenFrame) {
                    g_commandList->CopyResource(p.privateTexture.Get(), p.sharedTexture.Get());
                    p.lastSeenFrame = latestFrame;
                }
            }
        }

        std::vector<D3D12_RESOURCE_BARRIER> postCopyBarriers;
        for (const auto& b : preCopyBarriers) {
            D3D12_RESOURCE_BARRIER post = b;
            post.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            post.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postCopyBarriers.push_back(post);
        }
        if (!postCopyBarriers.empty()) g_commandList->ResourceBarrier((UINT)postCopyBarriers.size(), postCopyBarriers.data());

        // 2. Compose into g_muxCompositeTexture
        D3D12_RESOURCE_BARRIER compBarrier = {};
        compBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        compBarrier.Transition.pResource = g_muxCompositeTexture.Get();
        compBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        compBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        compBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &compBarrier);

        D3D12_CPU_DESCRIPTOR_HANDLE compRtv = g_muxCompositeRtvHeap->GetCPUDescriptorHandleForHeapStart();
        g_commandList->OMSetRenderTargets(1, &compRtv, FALSE, nullptr);
        const float clearCol[] = { 0.04f, 0.01f, 0.05f, 1.0f };
        g_commandList->ClearRenderTargetView(compRtv, clearCol, 0, nullptr);

        std::vector<int> activeProducers;
        for (int i = 0; i < MAX_MUX_PRODUCERS; ++i) {
            if (g_muxProducers[i].isConnected && g_muxProducers[i].privateTexture) {
                activeProducers.push_back(i);
            }
        }

        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = { g_muxSrvHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, heaps);
        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        if (!activeProducers.empty()) {
            g_commandList->SetPipelineState(g_pipelineStateBlit.Get());
            int count = (int)activeProducers.size();
            int cols = static_cast<int>(ceil(sqrt(static_cast<float>(count))));
            int rows = (count + cols - 1) / cols;

            for (int i = 0; i < count; ++i) {
                int pIdx = activeProducers[i];
                int gridCol = i % cols;
                int gridRow = i / cols;

                int left   = (gridCol * kDefaultWidth) / cols;
                int right  = ((gridCol + 1) * kDefaultWidth) / cols;
                int top    = (gridRow * kDefaultHeight) / rows;
                int bottom = ((gridRow + 1) * kDefaultHeight) / rows;

                D3D12_VIEWPORT vp = { (float)left, (float)top, (float)(right - left), (float)(bottom - top), 0.0f, 1.0f };
                D3D12_RECT sr = { left, top, right, bottom };

                g_commandList->RSSetViewports(1, &vp);
                g_commandList->RSSetScissorRects(1, &sr);

                D3D12_GPU_DESCRIPTOR_HANDLE srvHandle = g_muxSrvHeap->GetGPUDescriptorHandleForHeapStart();
                srvHandle.ptr += (UINT64)g_muxProducers[pIdx].srvDescriptorIndex * g_srvDescriptorSize;
                g_commandList->SetGraphicsRootDescriptorTable(1, srvHandle);
                g_commandList->DrawInstanced(3, 1, 0, 0);
            }
        } else {
            // Blueprint grid background when waiting for inputs
            g_commandList->SetPipelineState(g_pipelineStateBlueprint.Get());
            D3D12_VIEWPORT vp = { 0, 0, (float)kDefaultWidth, (float)kDefaultHeight, 0.0f, 1.0f };
            D3D12_RECT sr = { 0, 0, (LONG)kDefaultWidth, (LONG)kDefaultHeight };
            g_commandList->RSSetViewports(1, &vp);
            g_commandList->RSSetScissorRects(1, &sr);
            g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());
            g_commandList->DrawInstanced(3, 1, 0, 0);
        }

        // 3. Produce to g_muxSharedOutTexture
        D3D12_RESOURCE_BARRIER barriersToProduce[2] = {};
        barriersToProduce[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriersToProduce[0].Transition = { g_muxCompositeTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE };
        barriersToProduce[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriersToProduce[1].Transition = { g_muxSharedOutTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST };
        g_commandList->ResourceBarrier(2, barriersToProduce);

        g_commandList->CopyResource(g_muxSharedOutTexture.Get(), g_muxCompositeTexture.Get());

        D3D12_RESOURCE_BARRIER barriersAfterProduce[2] = {};
        barriersAfterProduce[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriersAfterProduce[0].Transition = { g_muxCompositeTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
        barriersAfterProduce[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriersAfterProduce[1].Transition = { g_muxSharedOutTexture.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON };
        g_commandList->ResourceBarrier(2, barriersAfterProduce);

        // 4. Preview to window backbuffer via blit
        D3D12_RESOURCE_BARRIER presentBarrier = {};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_commandList->ResourceBarrier(1, &presentBarrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtvH.ptr += (g_frameIndex * g_rtvDescriptorSize);
        g_commandList->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);

        g_commandList->SetPipelineState(g_pipelineStateBlit.Get());
        ID3D12DescriptorHeap* previewHeaps[] = { g_muxCompositeSrvHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, previewHeaps);
        g_commandList->SetGraphicsRootDescriptorTable(1, g_muxCompositeSrvHeap->GetGPUDescriptorHandleForHeapStart());

        D3D12_VIEWPORT winVp = { 0, 0, totalW, totalH, 0.0f, 1.0f };
        D3D12_RECT winSr = { 0, 0, (LONG)totalW, (LONG)totalH };
        g_commandList->RSSetViewports(1, &winVp);
        g_commandList->RSSetScissorRects(1, &winSr);
        g_commandList->DrawInstanced(3, 1, 0, 0);

        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &presentBarrier);
        return;
    }

    // --- NON-MULTIPLEXER MODES (Shader, Camera, Consumer) ---
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);

    D3D12_VIEWPORT winVp = { 0, 0, totalW, totalH, 0.0f, 1.0f };
    D3D12_RECT winSr = { 0, 0, (LONG)totalW, (LONG)totalH };
    g_commandList->RSSetViewports(1, &winVp);
    g_commandList->RSSetScissorRects(1, &winSr);

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        // Blit g_producerSharedTexture to window
        D3D12_RESOURCE_BARRIER readBarrier = {};
        readBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        readBarrier.Transition.pResource = g_producerSharedTexture.Get();
        readBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        readBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g_commandList->ResourceBarrier(1, &readBarrier);

        g_commandList->SetPipelineState(g_pipelineStateBlit.Get());
        ID3D12DescriptorHeap* heaps[] = { g_producerSharedSrvHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, heaps);
        g_commandList->SetGraphicsRootDescriptorTable(1, g_producerSharedSrvHeap->GetGPUDescriptorHandleForHeapStart());
        g_commandList->DrawInstanced(3, 1, 0, 0);

        readBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        readBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &readBarrier);
    } else if (g_mode == MODE_CONSUMER) {
        if (g_consumerSlot.active && g_consumerSlot.sharedTexture) {
            // Blit received shared texture to window
            D3D12_RESOURCE_BARRIER readBarrier = {};
            readBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            readBarrier.Transition.pResource = g_consumerSlot.sharedTexture.Get();
            readBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            readBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_commandList->ResourceBarrier(1, &readBarrier);

            g_commandList->SetPipelineState(g_pipelineStateBlit.Get());
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
            g_commandList->SetDescriptorHeaps(1, heaps);
            g_commandList->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());
            g_commandList->DrawInstanced(3, 1, 0, 0);

            readBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            readBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
            g_commandList->ResourceBarrier(1, &readBarrier);
        } else {
            // Awaiting stream: render blueprint grid so screen is never black!
            g_commandList->SetPipelineState(g_pipelineStateBlueprint.Get());
            g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());
            g_commandList->DrawInstanced(3, 1, 0, 0);
        }
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);
}

// Stage 3: Present & Sync
void StepGraph_PresentAndSync() {
    g_commandList->Close();
    ID3D12CommandList* lists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, lists);

    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        g_producerSharedFrameValue++;
        g_commandQueue->Signal(g_producerSharedFence.Get(), g_producerSharedFrameValue);
        if (g_pProducerManifestView) {
            g_pProducerManifestView->frameValue = g_producerSharedFrameValue;
            WakeByAddressAll(&g_pProducerManifestView->frameValue);
        }
    } else if (g_mode == MODE_MULTIPLEXER) {
        g_muxSharedOutFrameValue++;
        g_commandQueue->Signal(g_muxSharedOutFence.Get(), g_muxSharedOutFrameValue);
        if (g_pMuxManifestViewOut) {
            g_pMuxManifestViewOut->frameValue = g_muxSharedOutFrameValue;
            WakeByAddressAll(&g_pMuxManifestViewOut->frameValue);
        }
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

void WaitForGpu() {
    if (!g_commandQueue || !g_renderFence) return;
    const UINT64 value = g_fenceValues[g_frameIndex];
    g_commandQueue->Signal(g_renderFence.Get(), value);
    if (g_renderFence->GetCompletedValue() < value) {
        g_renderFence->SetEventOnCompletion(value, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
    g_fenceValues[g_frameIndex] = value + 1;
}

void ResizeSwapChain(UINT width, UINT height) {
    if (!g_swapChain || !width || !height) return;
    WaitForGpu();
    for (UINT i = 0; i < kFrameCount; ++i) {
        g_renderTargets[i].Reset();
        g_fenceValues[i] = g_fenceValues[g_frameIndex];
    }
    if (FAILED(g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN,
                                          DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT))) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
}

void StartAudioPlayback() {
    if (g_audioPlaying) return;
    g_audioPlaying = true;
    g_hAudioPlayStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hAudioPlayThread = CreateThread(nullptr, 0, AudioPlaybackThreadProc, nullptr, 0, nullptr);
}

void StopAudioPlayback() {
    if (g_audioPlaying) {
        g_audioPlaying = false;
        if (g_hAudioPlayStopEvent) SetEvent(g_hAudioPlayStopEvent);
        if (g_hAudioPlayThread) {
            WaitForSingleObject(g_hAudioPlayThread, INFINITE);
            CloseHandle(g_hAudioPlayThread);
            g_hAudioPlayThread = nullptr;
        }
        if (g_hAudioPlayStopEvent) {
            CloseHandle(g_hAudioPlayStopEvent);
            g_hAudioPlayStopEvent = nullptr;
        }
    }
}

DWORD WINAPI AudioPlaybackThreadProc(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        CoUninitialize();
        return 1;
    }

    ComPtr<IMMDevice> defaultDevice;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
        CoUninitialize();
        return 1;
    }

    ComPtr<IAudioClient> audioClient;
    if (FAILED(defaultDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient))) {
        CoUninitialize();
        return 1;
    }

    WAVEFORMATEX* pwfx = nullptr;
    if (FAILED(audioClient->GetMixFormat(&pwfx))) {
        CoUninitialize();
        return 1;
    }

    // Event-driven: the engine signals `ready` when it wants more samples.
    REFERENCE_TIME hnsBufferDuration = 2000000; // 200 ms
    if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, hnsBufferDuration, 0, pwfx, nullptr))) {
        CoTaskMemFree(pwfx);
        CoUninitialize();
        return 1;
    }
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ready || FAILED(audioClient->SetEventHandle(ready))) {
        if (ready) CloseHandle(ready);
        CoTaskMemFree(pwfx);
        CoUninitialize();
        return 1;
    }

    ComPtr<IAudioRenderClient> renderClient;
    if (FAILED(audioClient->GetService(IID_PPV_ARGS(&renderClient)))) {
        CoTaskMemFree(pwfx);
        CoUninitialize();
        return 1;
    }

    UINT32 bufferFrameCount = 0;
    audioClient->GetBufferSize(&bufferFrameCount);
    audioClient->Start();

    std::vector<float> sampleBuffer(bufferFrameCount * pwfx->nChannels);

    const HANDLE waits[] = { g_hAudioPlayStopEvent, ready };
    while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding))) break;

        UINT32 framesNeeded = bufferFrameCount - padding;
        if (framesNeeded > 0) {
            UINT32 samplesNeeded = framesNeeded * pwfx->nChannels;
            UINT32 samplesRead = g_audioRingConsumer.ReadSamples(sampleBuffer.data(), samplesNeeded);

            BYTE* pData = nullptr;
            if (SUCCEEDED(renderClient->GetBuffer(framesNeeded, &pData))) {
                if (samplesRead > 0) {
                    memcpy(pData, sampleBuffer.data(), samplesRead * sizeof(float));
                    if (samplesRead < samplesNeeded) {
                        memset(pData + (samplesRead * sizeof(float)), 0, (samplesNeeded - samplesRead) * sizeof(float));
                    }
                    renderClient->ReleaseBuffer(framesNeeded, 0);
                } else {
                    renderClient->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                }
            }
        }
    }

    audioClient->Stop();
    CloseHandle(ready);
    CoTaskMemFree(pwfx);
    CoUninitialize();
    return 0;
}

void Cleanup() {
    WaitForGpu();
    ManageTrayIcon(g_hwnd, false);
    TeardownProducerResources();
    TeardownCameraResources();
    TeardownConsumerResources();
    TeardownMuxResources();
    g_listener.Stop();

    if (g_pCbvData) g_constantBuffer->Unmap(0, nullptr);
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_TRAY_MSG:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                ShowContextMenu(hwnd, pt);
                return 0;
            } else if (lParam == WM_LBUTTONDBLCLK) {
                bool visible = IsWindowVisible(hwnd);
                ShowWindow(hwnd, visible ? SW_HIDE : SW_SHOW);
                if (!visible) SetForegroundWindow(hwnd);
                return 0;
            }
            break;

        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            ShowContextMenu(hwnd, pt);
            return 0;
        }

        case WM_APP_MENU_COMMAND:
            SendMessageW(hwnd, WM_COMMAND, wParam, lParam);
            return 0;

        case WM_COMMAND: {
            WORD cmd = LOWORD(wParam);
            if (cmd >= IDM_CAMERA_SELECT_BASE && cmd < IDM_CAMERA_SELECT_BASE + 32) {
                SwitchCamera(cmd - IDM_CAMERA_SELECT_BASE);
                return 0;
            }

            switch (cmd) {
                case IDM_MODE_SHADER: SwitchMode(MODE_PRODUCER_SHADER); return 0;
                case IDM_MODE_CAMERA: SwitchMode(MODE_PRODUCER_CAMERA); return 0;
                case IDM_MODE_CONSUMER: SwitchMode(MODE_CONSUMER); return 0;
                case IDM_MODE_MULTIPLEXER: SwitchMode(MODE_MULTIPLEXER); return 0;
                case IDM_CYCLE_CAMERA: {
                    auto cams = DirectPortCameraCapture::EnumerateCameras();
                    if (!cams.empty()) {
                        g_cameraDeviceIndex = (g_cameraDeviceIndex + 1) % (int)cams.size();
                        SwitchMode(MODE_PRODUCER_CAMERA);
                    }
                    return 0;
                }
                case IDM_RELOAD_SHADER: LoadProducerShader(g_shaderPath); return 0;
                case IDM_TOGGLE_AUDIO: ToggleAudio(); return 0;
                case IDM_SHOW_HIDE_WINDOW: {
                    bool visible = IsWindowVisible(hwnd);
                    ShowWindow(hwnd, visible ? SW_HIDE : SW_SHOW);
                    if (!visible) SetForegroundWindow(hwnd);
                    return 0;
                }
                case IDM_EXIT:
                    PostQuitMessage(0);
                    return 0;
            }
            break;
        }

        case WM_KEYDOWN:
            // Number row (and numpad) pick modes; 5 reloads the shader.
            if (wParam >= VK_NUMPAD1 && wParam <= VK_NUMPAD5) wParam = '1' + (wParam - VK_NUMPAD1);
            if (wParam == '1') { SwitchMode(MODE_PRODUCER_SHADER); return 0; }
            if (wParam == '2') { SwitchMode(MODE_PRODUCER_CAMERA); return 0; }
            if (wParam == '3') { SwitchMode(MODE_CONSUMER); return 0; }
            if (wParam == '4') { SwitchMode(MODE_MULTIPLEXER); return 0; }
            if (wParam == 'C') {
                auto cams = DirectPortCameraCapture::EnumerateCameras();
                if (!cams.empty()) {
                    g_cameraDeviceIndex = (g_cameraDeviceIndex + 1) % (int)cams.size();
                    SwitchMode(MODE_PRODUCER_CAMERA);
                }
                return 0;
            }
            if (wParam == '5' && g_mode == MODE_PRODUCER_SHADER) {
                LoadProducerShader(g_shaderPath);
                return 0;
            }
            if (wParam == 'M') {
                ToggleAudio();
                return 0;
            }
            if (wParam == VK_ESCAPE || wParam == 'Q') {
                PostQuitMessage(0);
                return 0;
            }
            break;

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED)
                ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
