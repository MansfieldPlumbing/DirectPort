// --- DirectPort.cpp ---
// Unified Industrial DirectPort D3D12 Utility.
// Linear Execution Graph with Single-Window Solution & System Tray Service.
// Features:
//  - Linear execution graph: Input Acquire -> Composition -> Present & Sync -> Audio Pump
//  - Windows System Tray (notification area) integration with custom context menu
//  - Hotkeys: F1 (Shader Producer), F2 (Camera Producer), F3 (Consumer), F4 (Multiplexer), F5 (Reload), M (Audio)
//  - Right-click window client area or system tray icon for instant mode switching
//  - Windows 11 Mica Alt backdrop (QuickPS attributes)
//  - Non-blocking UDP discovery beacon (3-second cadence, loopback zero-prompt default)
//  - Hardware crossbar queue wait (ID3D12CommandQueue::Wait, ~170ns latency)
//  - Event-driven waitable swapchain message pump (zero CPU thrashing)

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
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <algorithm>

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
#define WM_APP_TRAY_MSG       (WM_APP + 1)
#define IDM_MODE_SHADER       1001
#define IDM_MODE_CAMERA       1002
#define IDM_MODE_CONSUMER     1003
#define IDM_MODE_MULTIPLEXER  1004
#define IDM_RELOAD_SHADER     1005
#define IDM_TOGGLE_AUDIO      1006
#define IDM_SHOW_HIDE_WINDOW  1007
#define IDM_EXIT              1008

enum DirectPortAppMode {
    MODE_CONSUMER,
    MODE_PRODUCER_SHADER,
    MODE_PRODUCER_CAMERA,
    MODE_MULTIPLEXER
};

static const UINT kFrameCount = 2;
static const UINT kDefaultWidth = 1920;
static const UINT kDefaultHeight = 1080;
static const UINT kMaxMuxSlots = 4;

struct ShaderConstants {
    float u_resolution[4]; // xy = res, z = aspect, w = 0
    float u_time[4];       // x = elapsed, y = delta, z = frameIndex, w = 0
    float u_mouse[4];      // xy = pos, zw = 0
};

// --- Subsystem State ---
static HWND                           g_hwnd = nullptr;
static HINSTANCE                      g_instance = nullptr;
static HICON                          g_hIcon = nullptr;
static DirectPortAppMode              g_mode = MODE_CONSUMER;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_allocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_producerSharedRtvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_pipelineStateShader;
static ComPtr<ID3D12PipelineState>    g_pipelineStateBlit;
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
static std::wstring                   g_streamName = L"DirectPort_Main";
static std::wstring                   g_texHandleName = L"DirectPort_Tex_Main";
static std::wstring                   g_fenceHandleName = L"DirectPort_Fence_Main";
static std::wstring                   g_audioBufferName = L"DirectPort_Audio_Main";
static std::string                    g_shaderPath = "shaders/plasma.hlsl";
static ComPtr<ID3D12Resource>         g_cameraUploadBuffer;
static UINT8*                         g_pCameraUploadData = nullptr;
static auto                           g_producerStartTime = std::chrono::steady_clock::now();

// --- Consumer / Multiplexer Slot Struct ---
struct GraphStreamSlot {
    bool                   active = false;
    std::string            streamName;
    std::wstring           texHandleName;
    std::wstring           fenceHandleName;
    std::wstring           audioBufferName;
    UINT                   width = 0;
    UINT                   height = 0;
    DXGI_FORMAT            format = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Resource> sharedTexture;
    ComPtr<ID3D12Fence>    sharedFence;
    HANDLE                 hSharedTex = nullptr;
    HANDLE                 hSharedFence = nullptr;
    UINT64                 lastFrame = 0;
    bool                   hasAudio = false;
};

static GraphStreamSlot                g_consumerSlot;
static GraphStreamSlot                g_muxSlots[kMaxMuxSlots];

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
void TeardownConsumerResources();
void TeardownMuxResources();
void SwitchMode(DirectPortAppMode newMode);
void ToggleAudio();
void StepGraph_Discovery();
void StepGraph_InputAcquire();
void StepGraph_Composition(float totalW, float totalH);
void StepGraph_PresentAndSync();
void MoveToNextFrame();
void Cleanup();
void StartAudioPlayback();
void StopAudioPlayback();
DWORD WINAPI AudioPlaybackThreadProc(LPVOID);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    g_instance = hInstance;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--produce") == 0 || _wcsicmp(argv[i], L"-p") == 0) {
            g_mode = MODE_PRODUCER_SHADER;
            if (i + 1 < argc && argv[i + 1][0] != L'-') {
                char buf[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, buf, sizeof(buf), NULL, NULL);
                g_shaderPath = buf;
            }
        } else if (_wcsicmp(argv[i], L"--camera") == 0 || _wcsicmp(argv[i], L"-c") == 0) {
            g_mode = MODE_PRODUCER_CAMERA;
        } else if (_wcsicmp(argv[i], L"--consume") == 0 || _wcsicmp(argv[i], L"-s") == 0) {
            g_mode = MODE_CONSUMER;
        } else if (_wcsicmp(argv[i], L"--mux") == 0 || _wcsicmp(argv[i], L"-m") == 0) {
            g_mode = MODE_MULTIPLEXER;
        } else if (_wcsicmp(argv[i], L"--no-audio") == 0) {
            g_enableAudio = false;
        } else if (_wcsicmp(argv[i], L"--lan") == 0) {
            g_lanMode = true;
        }
    }
    LocalFree(argv);

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

        // Linear Graph: Discovery Cadence (every 3 seconds)
        StepGraph_Discovery();

        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
            RECT clientRc;
            GetClientRect(g_hwnd, &clientRc);
            float totalW = std::max(1.0f, (float)(clientRc.right - clientRc.left));
            float totalH = std::max(1.0f, (float)(clientRc.bottom - clientRc.top));

            g_allocators[g_frameIndex]->Reset();
            g_commandList->Reset(g_allocators[g_frameIndex].Get(), nullptr);

            StepGraph_InputAcquire();
            StepGraph_Composition(totalW, totalH);
            StepGraph_PresentAndSync();
        }
    }

    Cleanup();
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
    SetForegroundWindow(hwnd);

    auto* menu = new CustomMenu(hwnd, g_instance);
    menu->AddItem(L"Producer: Procedural Shader\tF1", IDM_MODE_SHADER, g_mode == MODE_PRODUCER_SHADER);
    menu->AddItem(L"Producer: Live Camera Feed\tF2", IDM_MODE_CAMERA, g_mode == MODE_PRODUCER_CAMERA);
    menu->AddItem(L"Consumer: Auto-Listen Stream\tF3", IDM_MODE_CONSUMER, g_mode == MODE_CONSUMER);
    menu->AddItem(L"Multiplexer: 4-Way Grid\tF4", IDM_MODE_MULTIPLEXER, g_mode == MODE_MULTIPLEXER);
    menu->AddSeparator();
    menu->AddItem(L"Reload HLSL Shader\tF5", IDM_RELOAD_SHADER, false);
    menu->AddItem(L"WASAPI Audio Crossbar\tM", IDM_TOGGLE_AUDIO, g_enableAudio);
    menu->AddSeparator();
    menu->AddItem(IsWindowVisible(hwnd) ? L"Hide Window" : L"Show Window", IDM_SHOW_HIDE_WINDOW, false);
    menu->AddItem(L"Exit DirectPort\tEsc", IDM_EXIT, false);

    int menuWidth = menu->GetCalculatedWidth();
    int menuHeight = menu->GetCalculatedHeight();

    HMONITOR hMonitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMonitor, &mi);

    int x = pt.x;
    int y = pt.y;
    if (x + menuWidth > mi.rcWork.right) x = pt.x - menuWidth;
    if (y + menuHeight > mi.rcWork.bottom) y = pt.y - menuHeight;

    menu->Show(x, y);
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
    UINT width = (UINT)std::max(1, (int)(rc.right - rc.left));
    UINT height = (UINT)std::max(1, (int)(rc.bottom - rc.top));

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

    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

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

    D3D12_RESOURCE_DESC camDesc = cbDesc;
    camDesc.Width = kDefaultWidth * kDefaultHeight * 4;
    g_device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &camDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_cameraUploadBuffer));
    g_cameraUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&g_pCameraUploadData));

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
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1].DescriptorTable.pDescriptorRanges = &range;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sigBlob;
    D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, nullptr);
    g_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));

    const char* blitShader = 
        "Texture2D g_texture : register(t0);\n"
        "SamplerState g_sampler : register(s0);\n"
        "struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
        "PSInput VSMain(uint id : SV_VertexID) {\n"
        "    PSInput o; o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o;\n"
        "}\n"
        "float4 PSMain(PSInput i) : SV_Target { return g_texture.Sample(g_sampler, i.uv); }\n";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(blitShader, strlen(blitShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(blitShader, strlen(blitShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);

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

    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineStateBlit));
    LoadProducerShader(g_shaderPath);
    return true;
}

bool LoadProducerShader(const std::string& path) {
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    bool isCso = (path.size() > 4 && path.substr(path.size() - 4) == ".cso");

    if (isCso) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        D3DCreateBlob((SIZE_T)size, &psBlob);
        file.read(reinterpret_cast<char*>(psBlob->GetBufferPointer()), size);
    } else {
        std::wstring wPath(path.begin(), path.end());
        HRESULT hr = D3DCompileFromFile(wPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, 
            "VSMain", "vs_5_0", 0, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) return false;

        hr = D3DCompileFromFile(wPath.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, 
            "PSMain", "ps_5_0", 0, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) return false;
    }

    if (!vsBlob) {
        const char* passthroughVS = 
            "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
            "VSOut VSMain(uint id : SV_VertexID) {\n"
            "    VSOut o; o.uv = float2((id << 1) & 2, id & 2);\n"
            "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); return o;\n"
            "}\n";
        D3DCompile(passthroughVS, strlen(passthroughVS), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
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
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;

    return SUCCEEDED(g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineStateShader)));
}

bool InitProducerSharedResources() {
    TeardownProducerResources();

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = kDefaultWidth;
    texDesc.Height = kDefaultHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_HEAP_PROPERTIES defaultHeap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_CLEAR_VALUE clearVal = {};
    clearVal.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    if (FAILED(g_device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_SHARED, &texDesc,
        D3D12_RESOURCE_STATE_COMMON, &clearVal, IID_PPV_ARGS(&g_producerSharedTexture)))) return false;

    g_device->CreateRenderTargetView(g_producerSharedTexture.Get(), nullptr, g_producerSharedRtvHeap->GetCPUDescriptorHandleForHeapStart());

    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_producerSharedFence)))) return false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

    g_device->CreateSharedHandle(g_producerSharedTexture.Get(), &sa, GENERIC_ALL, g_texHandleName.c_str(), &g_producerSharedTexHandle);
    g_device->CreateSharedHandle(g_producerSharedFence.Get(), &sa, GENERIC_ALL, g_fenceHandleName.c_str(), &g_producerSharedFenceHandle);

    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);

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
    g_cameraCapture.Shutdown();

    if (g_producerSharedTexHandle) { CloseHandle(g_producerSharedTexHandle); g_producerSharedTexHandle = nullptr; }
    if (g_producerSharedFenceHandle) { CloseHandle(g_producerSharedFenceHandle); g_producerSharedFenceHandle = nullptr; }
    g_producerSharedTexture.Reset();
    g_producerSharedFence.Reset();
}

void TeardownConsumerResources() {
    StopAudioPlayback();
    g_audioRingConsumer.Close();
    if (g_consumerSlot.hSharedTex) CloseHandle(g_consumerSlot.hSharedTex);
    if (g_consumerSlot.hSharedFence) CloseHandle(g_consumerSlot.hSharedFence);
    g_consumerSlot.sharedTexture.Reset();
    g_consumerSlot.sharedFence.Reset();
    g_consumerSlot.active = false;
}

void TeardownMuxResources() {
    for (UINT i = 0; i < kMaxMuxSlots; ++i) {
        if (g_muxSlots[i].hSharedTex) CloseHandle(g_muxSlots[i].hSharedTex);
        if (g_muxSlots[i].hSharedFence) CloseHandle(g_muxSlots[i].hSharedFence);
        g_muxSlots[i].sharedTexture.Reset();
        g_muxSlots[i].sharedFence.Reset();
        g_muxSlots[i].active = false;
    }
}

void SwitchMode(DirectPortAppMode newMode) {
    TeardownProducerResources();
    TeardownConsumerResources();
    TeardownMuxResources();

    g_mode = newMode;

    switch (g_mode) {
        case MODE_PRODUCER_SHADER:
            InitProducerSharedResources();
            SetWindowTextW(g_hwnd, L"DirectPort [PRODUCER: Shader] // Right-click or tray for menu");
            break;
        case MODE_PRODUCER_CAMERA:
            if (!g_cameraCapture.Initialize(0)) {
                SwitchMode(MODE_PRODUCER_SHADER);
                return;
            }
            InitProducerSharedResources();
            SetWindowTextW(g_hwnd, L"DirectPort [PRODUCER: Camera] // Right-click or tray for menu");
            break;
        case MODE_CONSUMER:
            SetWindowTextW(g_hwnd, L"DirectPort [CONSUMER: Listening...] // Right-click or tray for menu");
            break;
        case MODE_MULTIPLEXER:
            SetWindowTextW(g_hwnd, L"DirectPort [MULTIPLEXER: 4-Way Crossbar] // Right-click or tray for menu");
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

// Stage 0: Discovery Cadence (Every 3 seconds)
void StepGraph_Discovery() {
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - g_lastDiscoveryCheck).count() < 3) return;
    g_lastDiscoveryCheck = now;

    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        g_broadcaster.Broadcast("DirectPort_Main", kDefaultWidth, kDefaultHeight, DXGI_FORMAT_R8G8B8A8_UNORM,
            g_texHandleName.c_str(), g_fenceHandleName.c_str(), g_producerSharedFrameValue,
            g_enableAudio, 48000, 2, g_audioBufferName.c_str());
    } else if (g_mode == MODE_CONSUMER) {
        g_listener.Poll(g_discoveredStreams);
        if (!g_consumerSlot.active && !g_discoveredStreams.empty()) {
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

                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = 1;
                g_device->CreateShaderResourceView(g_consumerSlot.sharedTexture.Get(), &srvDesc, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

                if (g_enableAudio && d.hasAudio && !d.audioBufferName.empty()) {
                    if (g_audioRingConsumer.Open(d.audioBufferName.c_str())) StartAudioPlayback();
                }

                std::wstring title = L"DirectPort [CONSUMER: " + std::wstring(d.streamName.begin(), d.streamName.end()) + L"]" + (d.hasAudio ? L" [A/V]" : L"");
                SetWindowTextW(g_hwnd, title.c_str());
            }
        }
    } else if (g_mode == MODE_MULTIPLEXER) {
        g_listener.Poll(g_discoveredStreams);
        for (UINT i = 0; i < kMaxMuxSlots; ++i) {
            if (i < g_discoveredStreams.size()) {
                const auto& d = g_discoveredStreams[i];
                auto& slot = g_muxSlots[i];
                if (!slot.active || slot.texHandleName != d.textureHandleName) {
                    if (slot.hSharedTex) CloseHandle(slot.hSharedTex);
                    if (slot.hSharedFence) CloseHandle(slot.hSharedFence);
                    slot.sharedTexture.Reset();
                    slot.sharedFence.Reset();

                    HANDLE hTex = nullptr, hFence = nullptr;
                    if (SUCCEEDED(g_device->OpenSharedHandleByName(d.textureHandleName.c_str(), GENERIC_ALL, &hTex)) &&
                        SUCCEEDED(g_device->OpenSharedHandleByName(d.fenceHandleName.c_str(), GENERIC_ALL, &hFence))) {
                        g_device->OpenSharedHandle(hTex, IID_PPV_ARGS(&slot.sharedTexture));
                        g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&slot.sharedFence));
                        slot.hSharedTex = hTex;
                        slot.hSharedFence = hFence;
                        slot.texHandleName = d.textureHandleName;
                        slot.streamName = d.streamName;
                        slot.active = true;

                        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = 1;
                        D3D12_CPU_DESCRIPTOR_HANDLE cpuH = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
                        cpuH.ptr += (i * g_srvDescriptorSize);
                        g_device->CreateShaderResourceView(slot.sharedTexture.Get(), &srvDesc, cpuH);
                    }
                }
            }
        }
    }
}

// Stage 1: Input Acquire
void StepGraph_InputAcquire() {
    if (g_mode == MODE_PRODUCER_SHADER) {
        g_commandList->SetPipelineState(g_pipelineStateShader.Get());
        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());

        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - g_producerStartTime).count();
        g_pCbvData->u_resolution[0] = (float)kDefaultWidth;
        g_pCbvData->u_resolution[1] = (float)kDefaultHeight;
        g_pCbvData->u_resolution[2] = (float)kDefaultWidth / (float)kDefaultHeight;
        g_pCbvData->u_time[0] = elapsed;
        g_pCbvData->u_time[2] = (float)g_producerSharedFrameValue;

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

        g_commandList->SetGraphicsRootConstantBufferView(0, g_constantBuffer->GetGPUVirtualAddress());
        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_commandList->DrawInstanced(3, 1, 0, 0);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        g_commandList->ResourceBarrier(1, &barrier);
    } else if (g_mode == MODE_PRODUCER_CAMERA && g_cameraCapture.IsActive()) {
        std::vector<BYTE> camBytes;
        if (g_cameraCapture.ReadFrame(camBytes) && g_pCameraUploadData) {
            memcpy(g_pCameraUploadData, camBytes.data(), std::min(camBytes.size(), (size_t)(kDefaultWidth * kDefaultHeight * 4)));
        }

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = g_producerSharedTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_commandList->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
        dst.pResource = g_producerSharedTexture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
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
    } else if (g_mode == MODE_CONSUMER) {
        if (g_consumerSlot.active && g_consumerSlot.sharedFence) {
            UINT64 frame = g_consumerSlot.sharedFence->GetCompletedValue();
            if (frame > g_consumerSlot.lastFrame) {
                g_commandQueue->Wait(g_consumerSlot.sharedFence.Get(), frame);
                g_consumerSlot.lastFrame = frame;
            }
        }
    } else if (g_mode == MODE_MULTIPLEXER) {
        for (UINT i = 0; i < kMaxMuxSlots; ++i) {
            auto& slot = g_muxSlots[i];
            if (slot.active && slot.sharedFence) {
                UINT64 frame = slot.sharedFence->GetCompletedValue();
                if (frame > slot.lastFrame) {
                    g_commandQueue->Wait(slot.sharedFence.Get(), frame);
                    slot.lastFrame = frame;
                }
            }
        }
    }
}

// Stage 2: Composition
void StepGraph_Composition(float totalW, float totalH) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvH.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvH, FALSE, nullptr);

    const float clearColor[] = { 0.04f, 0.04f, 0.06f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvH, clearColor, 0, nullptr);

    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
        copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[0].Transition.pResource = g_producerSharedTexture.Get();
        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;

        copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        copyBarriers[1].Transition.pResource = g_renderTargets[g_frameIndex].Get();
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_commandList->ResourceBarrier(2, copyBarriers);

        g_commandList->CopyResource(g_renderTargets[g_frameIndex].Get(), g_producerSharedTexture.Get());

        copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(2, copyBarriers);
    } else if (g_mode == MODE_CONSUMER) {
        if (g_consumerSlot.active && g_consumerSlot.sharedTexture) {
            D3D12_VIEWPORT vp = { 0, 0, totalW, totalH, 0.0f, 1.0f };
            D3D12_RECT sc = { 0, 0, (LONG)totalW, (LONG)totalH };
            g_commandList->RSSetViewports(1, &vp);
            g_commandList->RSSetScissorRects(1, &sc);

            g_commandList->SetPipelineState(g_pipelineStateBlit.Get());
            g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());

            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
            g_commandList->SetDescriptorHeaps(1, heaps);
            g_commandList->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

            g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            g_commandList->DrawInstanced(3, 1, 0, 0);
        }
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);
    } else if (g_mode == MODE_MULTIPLEXER) {
        float halfW = totalW / 2.0f;
        float halfH = totalH / 2.0f;
        D3D12_VIEWPORT vps[4] = {
            { 0,     0,     halfW, halfH, 0.0f, 1.0f },
            { halfW, 0,     halfW, halfH, 0.0f, 1.0f },
            { 0,     halfH, halfW, halfH, 0.0f, 1.0f },
            { halfW, halfH, halfW, halfH, 0.0f, 1.0f }
        };

        g_commandList->SetPipelineState(g_pipelineStateBlit.Get());
        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());

        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, heaps);
        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        for (UINT i = 0; i < kMaxMuxSlots; ++i) {
            auto& slot = g_muxSlots[i];
            if (slot.active && slot.sharedTexture) {
                D3D12_RECT sc = { (LONG)vps[i].TopLeftX, (LONG)vps[i].TopLeftY, 
                                  (LONG)(vps[i].TopLeftX + vps[i].Width), (LONG)(vps[i].TopLeftY + vps[i].Height) };
                g_commandList->RSSetViewports(1, &vps[i]);
                g_commandList->RSSetScissorRects(1, &sc);

                D3D12_GPU_DESCRIPTOR_HANDLE gpuH = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
                gpuH.ptr += (i * g_srvDescriptorSize);
                g_commandList->SetGraphicsRootDescriptorTable(1, gpuH);
                g_commandList->DrawInstanced(3, 1, 0, 0);
            }
        }
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_commandList->ResourceBarrier(1, &barrier);
    }
}

// Stage 3: Present & Sync
void StepGraph_PresentAndSync() {
    g_commandList->Close();
    ID3D12CommandList* lists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, lists);

    if (g_mode == MODE_PRODUCER_SHADER || g_mode == MODE_PRODUCER_CAMERA) {
        g_producerSharedFrameValue++;
        g_commandQueue->Signal(g_producerSharedFence.Get(), g_producerSharedFrameValue);
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

// --- WASAPI Audio Playback ---
void StartAudioPlayback() {
    StopAudioPlayback();
    g_hAudioPlayStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_audioPlaying = true;
    g_hAudioPlayThread = CreateThread(nullptr, 0, AudioPlaybackThreadProc, nullptr, 0, nullptr);
}

void StopAudioPlayback() {
    if (g_audioPlaying) {
        g_audioPlaying = false;
        if (g_hAudioPlayStopEvent) SetEvent(g_hAudioPlayStopEvent);
        if (g_hAudioPlayThread) {
            WaitForSingleObject(g_hAudioPlayThread, 1000);
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

    REFERENCE_TIME hnsBufferDuration = 10000000;
    if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, pwfx, nullptr))) {
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

    while (WaitForSingleObject(g_hAudioPlayStopEvent, 10) == WAIT_TIMEOUT) {
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
    CoTaskMemFree(pwfx);
    CoUninitialize();
    return 0;
}

void Cleanup() {
    ManageTrayIcon(g_hwnd, false);
    TeardownProducerResources();
    TeardownConsumerResources();
    TeardownMuxResources();
    g_listener.Stop();

    if (g_pCbvData) g_constantBuffer->Unmap(0, nullptr);
    if (g_pCameraUploadData) g_cameraUploadBuffer->Unmap(0, nullptr);
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

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_MODE_SHADER: SwitchMode(MODE_PRODUCER_SHADER); return 0;
                case IDM_MODE_CAMERA: SwitchMode(MODE_PRODUCER_CAMERA); return 0;
                case IDM_MODE_CONSUMER: SwitchMode(MODE_CONSUMER); return 0;
                case IDM_MODE_MULTIPLEXER: SwitchMode(MODE_MULTIPLEXER); return 0;
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

        case WM_KEYDOWN:
            if (wParam == VK_F1) { SwitchMode(MODE_PRODUCER_SHADER); return 0; }
            if (wParam == VK_F2) { SwitchMode(MODE_PRODUCER_CAMERA); return 0; }
            if (wParam == VK_F3) { SwitchMode(MODE_CONSUMER); return 0; }
            if (wParam == VK_F4) { SwitchMode(MODE_MULTIPLEXER); return 0; }
            if (wParam == VK_F5 && g_mode == MODE_PRODUCER_SHADER) {
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

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
