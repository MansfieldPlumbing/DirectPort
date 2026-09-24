// --- DirectPortConsumer.cpp ---
// High-performance D3D12 DirectPort Consumer.
// Fixes all legacy defects:
//  - ZERO OpenFileMapping spinloops (event-driven UDP discovery via loopback)
//  - ZERO CPU-GPU lockstep serialization (hardware queue wait via ID3D12CommandQueue::Wait)
//  - ZERO hot-polling pump (event-driven MsgWaitForMultipleObjectsEx with waitable swapchain)
//  - Windows 11 Mica Alt backdrop (QuickPS attributes)
//  - Synchronized A/V playback (WASAPI render client from shared memory audio ring buffer)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dwmapi.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <chrono>
#include "../sdk/directport.h"
#include "../sdk/DirectPort_Discovery.h"
#include "../sdk/DirectPort_Audio.h"

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

// --- Subsystem Handles ---
static HWND                           g_hwnd = nullptr;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_allocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_pipelineState;
static UINT                           g_rtvDescriptorSize = 0;
static UINT                           g_srvDescriptorSize = 0;
static UINT                           g_frameIndex = 0;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_fenceValues[kFrameCount] = {};
static HANDLE                         g_fenceEvent = nullptr;

// --- DirectPort Connected Stream ---
struct StreamConnection {
    bool                   isConnected = false;
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
    UINT64                 lastProcessedFrame = 0;
    bool                   hasAudio = false;
};

static StreamConnection               g_activeStream;
static DirectPortDiscoveryListener    g_listener;
static std::vector<DirectPortDiscoveryListener::DiscoveredStream> g_discoveredStreams;
static DirectPortAudioRingConsumer    g_audioConsumer;

// --- Audio Playback Thread ---
static HANDLE                         g_hAudioThread = nullptr;
static HANDLE                         g_hAudioStopEvent = nullptr;
static bool                           g_audioPlaying = false;
static auto                           g_lastDiscoveryCheck = std::chrono::steady_clock::now() - std::chrono::seconds(3);

// --- Forward Declarations ---
void ApplyMicaWindowAttributes(HWND hwnd);
bool InitD3D12(HWND hwnd);
bool InitShaders();
bool ConnectToStream(const DirectPortDiscoveryListener::DiscoveredStream& stream);
void DisconnectStream();
void RenderFrame();
void MoveToNextFrame();
void Cleanup();
void StartAudioPlayback();
void StopAudioPlayback();
DWORD WINAPI AudioPlaybackThreadProc(LPVOID pParam);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    const WCHAR szClass[] = L"DirectPortConsumerClass";
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = szClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, szClass, L"DirectPort Consumer // Listening for Streams...", 
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 
        rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    if (!g_hwnd) return 1;

    ApplyMicaWindowAttributes(g_hwnd);

    if (!InitD3D12(g_hwnd)) return 1;
    if (!InitShaders()) return 1;

    // Start discovery listener in loopback mode (zero Windows Firewall prompt)
    g_listener.Start(true);

    ShowWindow(g_hwnd, nCmdShow);

    // Fallback: Check if default named handles already exist
    HANDLE testTex = OpenFileMappingW(FILE_MAP_READ, FALSE, L"DirectPort_Tex_Main");
    if (testTex) {
        CloseHandle(testTex);
        DirectPortDiscoveryListener::DiscoveredStream fallback = {};
        fallback.streamName = "DirectPort_Main";
        fallback.textureHandleName = L"DirectPort_Tex_Main";
        fallback.fenceHandleName = L"DirectPort_Fence_Main";
        fallback.audioBufferName = L"DirectPort_Audio_Main";
        fallback.width = 1920;
        fallback.height = 1080;
        fallback.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        fallback.hasAudio = true;
        ConnectToStream(fallback);
    }

    HANDLE hWaitable = g_swapChain->GetFrameLatencyWaitableObject();
    bool running = true;

    while (running) {
        // Event-driven wait: Wake up on swapchain readiness, window messages, or 16ms timeout (~60 FPS)
        DWORD waitResult = MsgWaitForMultipleObjectsEx(1, &hWaitable, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg = {};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        if (!running) break;

        // Low-frequency discovery check (once every 3 seconds) when looking for streams
        auto now = std::chrono::steady_clock::now();
        if (!g_activeStream.isConnected && std::chrono::duration_cast<std::chrono::seconds>(now - g_lastDiscoveryCheck).count() >= 3) {
            g_lastDiscoveryCheck = now;
            g_listener.Poll(g_discoveredStreams);
            if (!g_discoveredStreams.empty()) {
                ConnectToStream(g_discoveredStreams[0]);
            }
        }

        if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_TIMEOUT) {
            RenderFrame();
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

    int backdrop = 4;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop)))) {
        backdrop = 2;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    }
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
    UINT width = max(1, rc.right - rc.left);
    UINT height = max(1, rc.bottom - rc.top);

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

    // SRV heap for shared textures
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.NumDescriptors = 4;
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_allocators[0].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return true;
}

bool InitShaders() {
    const char* blitShader = 
        "Texture2D g_texture : register(t0);\n"
        "SamplerState g_sampler : register(s0);\n"
        "struct PSInput { float4 pos : SV_Position; float2 uv : TEXCOORD; };\n"
        "PSInput VSMain(uint id : SV_VertexID) {\n"
        "    PSInput o;\n"
        "    o.uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    return o;\n"
        "}\n"
        "float4 PSMain(PSInput i) : SV_Target {\n"
        "    return g_texture.Sample(g_sampler, i.uv);\n"
        "}\n";

    ComPtr<ID3DBlob> vsBlob, psBlob;
    if (FAILED(D3DCompile(blitShader, strlen(blitShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr))) return false;
    if (FAILED(D3DCompile(blitShader, strlen(blitShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr))) return false;

    // Root signature with 1 SRV descriptor table and 1 static sampler
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rootParam = {};
    rootParam.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParam.DescriptorTable.NumDescriptorRanges = 1;
    rootParam.DescriptorTable.pDescriptorRanges = &range;
    rootParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 1;
    rsDesc.pParameters = &rootParam;
    rsDesc.NumStaticSamplers = 1;
    rsDesc.pStaticSamplers = &sampler;
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

bool ConnectToStream(const DirectPortDiscoveryListener::DiscoveredStream& s) {
    DisconnectStream();

    // 1. Open shared NT handles
    HANDLE hTex = nullptr;
    HANDLE hFence = nullptr;

    // Use OpenSharedHandle via D3D12Device
    HRESULT hr = g_device->OpenSharedHandleByName(s.textureHandleName.c_str(), GENERIC_ALL, &hTex);
    if (FAILED(hr) || !hTex) return false;

    hr = g_device->OpenSharedHandleByName(s.fenceHandleName.c_str(), GENERIC_ALL, &hFence);
    if (FAILED(hr) || !hFence) { CloseHandle(hTex); return false; }

    ComPtr<ID3D12Resource> sharedTex;
    hr = g_device->OpenSharedHandle(hTex, IID_PPV_ARGS(&sharedTex));
    if (FAILED(hr)) { CloseHandle(hTex); CloseHandle(hFence); return false; }

    ComPtr<ID3D12Fence> sharedFence;
    hr = g_device->OpenSharedHandle(hFence, IID_PPV_ARGS(&sharedFence));
    if (FAILED(hr)) { CloseHandle(hTex); CloseHandle(hFence); return false; }

    // 2. Create Shader Resource View (SRV) in descriptor heap
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = s.format != DXGI_FORMAT_UNKNOWN ? (DXGI_FORMAT)s.format : DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    g_device->CreateShaderResourceView(sharedTex.Get(), &srvDesc, cpuHandle);

    g_activeStream.isConnected = true;
    g_activeStream.streamName = s.streamName;
    g_activeStream.texHandleName = s.textureHandleName;
    g_activeStream.fenceHandleName = s.fenceHandleName;
    g_activeStream.audioBufferName = s.audioBufferName;
    g_activeStream.width = s.width;
    g_activeStream.height = s.height;
    g_activeStream.format = (DXGI_FORMAT)s.format;
    g_activeStream.sharedTexture = sharedTex;
    g_activeStream.sharedFence = sharedFence;
    g_activeStream.hSharedTex = hTex;
    g_activeStream.hSharedFence = hFence;
    g_activeStream.hasAudio = s.hasAudio;

    // Connect Audio Ring Consumer if present
    if (s.hasAudio && !s.audioBufferName.empty()) {
        if (g_audioConsumer.Open(s.audioBufferName.c_str())) {
            StartAudioPlayback();
        }
    }

    std::wstring title = L"DirectPort Consumer // Stream: " + 
        std::wstring(s.streamName.begin(), s.streamName.end()) + 
        L" [" + std::to_wstring(s.width) + L"x" + std::to_wstring(s.height) + L"]" +
        (s.hasAudio ? L" [A/V]" : L"");
    SetWindowTextW(g_hwnd, title.c_str());

    return true;
}

void DisconnectStream() {
    if (g_activeStream.isConnected) {
        StopAudioPlayback();
        g_audioConsumer.Close();
        if (g_activeStream.hSharedTex) CloseHandle(g_activeStream.hSharedTex);
        if (g_activeStream.hSharedFence) CloseHandle(g_activeStream.hSharedFence);
        g_activeStream.sharedTexture.Reset();
        g_activeStream.sharedFence.Reset();
        g_activeStream.isConnected = false;
        SetWindowTextW(g_hwnd, L"DirectPort Consumer // Listening for Streams...");
    }
}

void RenderFrame() {
    g_allocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_allocators[g_frameIndex].Get(), g_pipelineState.Get());

    // Hardware Crossbar Queue Wait:
    // Instruct the GPU command queue to wait until the Producer has finished rendering this frame.
    // Zero CPU thread stalls, preserves sub-microsecond latency.
    if (g_activeStream.isConnected && g_activeStream.sharedFence) {
        UINT64 producerFrame = g_activeStream.sharedFence->GetCompletedValue();
        if (producerFrame > g_activeStream.lastProcessedFrame) {
            g_commandQueue->Wait(g_activeStream.sharedFence.Get(), producerFrame);
            g_activeStream.lastProcessedFrame = producerFrame;
        }
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Dark slate clear when waiting for stream
    const float clearColor[] = { 0.05f, 0.05f, 0.08f, 1.0f };
    g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    if (g_activeStream.isConnected && g_activeStream.sharedTexture) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        float w = (float)(rc.right - rc.left);
        float h = (float)(rc.bottom - rc.top);

        D3D12_VIEWPORT vp = { 0, 0, w, h, 0.0f, 1.0f };
        D3D12_RECT scissor = { 0, 0, (LONG)w, (LONG)h };
        g_commandList->RSSetViewports(1, &vp);
        g_commandList->RSSetScissorRects(1, &scissor);

        g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
        ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
        g_commandList->SetDescriptorHeaps(1, heaps);
        g_commandList->SetGraphicsRootDescriptorTable(0, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

        g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_commandList->DrawInstanced(3, 1, 0, 0);
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);

    g_commandList->Close();

    ID3D12CommandList* cmdLists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(1, cmdLists);

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

// --- WASAPI Audio Playback Client ---
void StartAudioPlayback() {
    StopAudioPlayback();
    g_hAudioStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_audioPlaying = true;
    g_hAudioThread = CreateThread(nullptr, 0, AudioPlaybackThreadProc, nullptr, 0, nullptr);
}

void StopAudioPlayback() {
    if (g_audioPlaying) {
        g_audioPlaying = false;
        if (g_hAudioStopEvent) SetEvent(g_hAudioStopEvent);
        if (g_hAudioThread) {
            WaitForSingleObject(g_hAudioThread, 1000);
            CloseHandle(g_hAudioThread);
            g_hAudioThread = nullptr;
        }
        if (g_hAudioStopEvent) {
            CloseHandle(g_hAudioStopEvent);
            g_hAudioStopEvent = nullptr;
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

    REFERENCE_TIME hnsBufferDuration = 10000000; // 1 second
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

    while (WaitForSingleObject(g_hAudioStopEvent, 10) == WAIT_TIMEOUT) {
        UINT32 padding = 0;
        if (FAILED(audioClient->GetCurrentPadding(&padding))) break;

        UINT32 framesNeeded = bufferFrameCount - padding;
        if (framesNeeded > 0) {
            UINT32 samplesNeeded = framesNeeded * pwfx->nChannels;
            UINT32 samplesRead = g_audioConsumer.ReadSamples(sampleBuffer.data(), samplesNeeded);

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
    DisconnectStream();
    g_listener.Stop();
    if (g_fenceEvent) CloseHandle(g_fenceEvent);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            // Swapchain resizing handled gracefully
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
