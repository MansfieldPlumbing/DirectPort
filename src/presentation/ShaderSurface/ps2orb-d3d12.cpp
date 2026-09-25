// --- DirectPortProducerD3D12.cpp ---

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <intrin.h>
#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "Synchronization.lib")

using namespace Microsoft::WRL;

// --- Undocumented user32.dll structures for forced hardware blur ---
enum ACCENT_STATE {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4
};

struct ACCENT_POLICY {
    ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
};

struct WINDOWCOMPOSITIONATTRIBDATA {
    DWORD Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

typedef BOOL(WINAPI* pSetWindowCompositionAttribute)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);
// -------------------------------------------------------------------

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

void Log(const std::wstring& msg) {
    WCHAR buffer[1024];
    DWORD pid = GetCurrentProcessId();
    wsprintfW(buffer, L"[PID:%lu][D3D12_Producer] %s\n", pid, msg.c_str());
    OutputDebugStringW(buffer);
}
void LogHRESULT(const std::wstring& msg, HRESULT hr) { 
    WCHAR b[512]; 
    wsprintfW(b, L"[D3D12_Producer] %s - HRESULT: 0x%08X\n", msg.c_str(), hr); 
    OutputDebugStringW(b); 
}

static const UINT kFrameCount = 2;
static ComPtr<ID3D12Device>           g_device;
static ComPtr<ID3D12CommandQueue>     g_commandQueue;
static ComPtr<IDXGISwapChain3>        g_swapChain;
static ComPtr<ID3D12Resource>         g_renderTargets[kFrameCount];
static ComPtr<ID3D12CommandAllocator> g_commandAllocators[kFrameCount];
static ComPtr<ID3D12GraphicsCommandList> g_commandList;
static ComPtr<ID3D12DescriptorHeap>   g_rtvHeap;
static ComPtr<ID3D12DescriptorHeap>   g_srvHeap;
static ComPtr<ID3D12RootSignature>    g_rootSignature;
static ComPtr<ID3D12PipelineState>    g_pipelineState;
static ComPtr<ID3D12PipelineState>    g_passthroughPSO;
static UINT                           g_rtvDescriptorSize;
static UINT                           g_srvDescriptorSize;
static UINT                           g_frameIndex;
static ComPtr<ID3D12Fence>            g_renderFence;
static UINT64                         g_renderFenceValues[kFrameCount] = {};
static HWND                           g_hwnd;
static HANDLE                         g_fenceEvent;
static UINT64                         g_fenceValue = 1;

// Matched perfectly with HLSL cbuffer layout (12 floats = 48 bytes)
struct ConstantBuffer { 
    float resolution[2];
    float time;
    float transitionProgress;
    float mouse[4];
    float dots;
    float orbScale;
    float padding[2];
};

static ComPtr<ID3D12Resource>         g_constantBuffer;
static UINT8*                         g_pCbvDataBegin = nullptr;
static auto gStartTime = std::chrono::high_resolution_clock::now();

static ComPtr<ID3D12Resource>         g_sharedTexture;
static ComPtr<ID3D12DescriptorHeap>   g_sharedRtvHeap;
static ComPtr<ID3D12Fence>            g_sharedFence;
static UINT64                         g_sharedFrameValue = 0;

static HANDLE                         g_hManifest = nullptr;
static BroadcastManifest*             g_pManifestView = nullptr;
static std::wstring                   g_sharedTextureName, g_sharedFenceName;
static HANDLE                         g_sharedTextureHandle = nullptr;
static HANDLE                         g_sharedFenceHandle = nullptr;

void InitD3D12(HWND hwnd);
void LoadAssets();
void PopulateCommandList();
void MoveToNextFrame();
void RenderFrame();
HRESULT InitializeSharing(UINT width, UINT height);
void ShutdownSharing();
void Cleanup();
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void WaitForGpuIdle();
void OnResize(UINT width, UINT height);
void EnablePersistentAcrylic(HWND hwnd);

// Merged the HLSL PS2 Orb code with Full-Screen Triangle Generation
const char* g_GenerationShaderHLSL = R"(
    cbuffer Constants : register(b0)
    {
        float2 iResolution;
        float iTime;
        float iTransitionProgress;
        float4 iMouse;
        float u_dots;
        float u_orbScale;
        float2 padding;
    };

    struct PS_INPUT
    {
        float4 pos : SV_POSITION;
        float2 uv  : TEXCOORD0;
    };

    PS_INPUT VSMain(uint id : SV_VertexID)
    {
        PS_INPUT output;
        float2 uv = float2((id << 1) & 2, id & 2);
        output.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        output.uv = uv;
        return output;
    }

    #define clamps(x) clamp(x, 0.0, 1.0)

    static const float pi = 3.14159265358979323;
    static const float xSpeed = 0.52;
    static const float ySpeed = 1.1;
    static const float zSpeed = 2.15;

    float3x3 r(float a) 
    {
        float ax = xSpeed * a;
        float ay = ySpeed * a;
        float az = zSpeed * a;
        
        float cy = cos(ay), sy = sin(ay);
        float cz = cos(az), sz = sin(az);
        float cx = cos(ax), sx = sin(ax);
        float sxsz = sx * sz, cxsz = cx * sz;
        
        return float3x3(
            cz * cy, -sz, cz * sy,
            (cxsz * cy + sx * sy), cx * cz, (cxsz * sy - sx * cy),
            (sxsz * cy - cx * sy), sx * cz, (sxsz * sy - cx * cy)
        );
    }

    float3 dirDist(float dir, float dist) 
    {
        return float3(cos(dir) * dist, sin(dir) * dist, 0.0);
    }

    float segment_distance_square(float2 v, float2 w, float2 p) 
    {
        float2 dvec = v - w;
        float l2 = dot(dvec, dvec);
        float t = clamps(dot(p - v, w - v) / max(l2, 0.000001));
        float2 projection = v + t * (w - v);
        float2 distVec = p - projection;
        return dot(distVec, distVec);
    }

    float4 PSMain(PS_INPUT input) : SV_TARGET
    {
        float2 uv = input.uv;
        float2 suv = uv - 0.5;
        suv.y = -suv.y; 
        suv.x *= iResolution.x / iResolution.y;
        
        float orbScale = u_orbScale > 0.0 ? u_orbScale : 1.0;
        suv /= orbScale;
        
        float3 currentHead = float3(0.0, 0.0, 0.0);
        float3 accumulatedTrail = float3(0.0, 0.0, 0.0);
        
        float DISTANCE = 0.2;
        int TRAIL_STEPS = 25;
        float TRAIL_LENGTH = 1.6;
        float stepDelta = TRAIL_LENGTH / float(TRAIL_STEPS);
        
        float dotsVal = u_dots > 0.0 ? u_dots : 8.0; 
        
        for (int i = 0; i < TRAIL_STEPS; i++) 
        {
            float f = float(i);
            float time1 = iTime - f * stepDelta;
            float time2 = iTime - (f + 1.0) * stepDelta;
            
            float3x3 r1 = r(time1);
            float3x3 r2 = r(time2);
            
            float circles_sq = 1000.0;
            
            for (float k = 0.0; k < 20.0; k++) 
            {
                if (k >= dotsVal) break;
                
                float pSpeed = k * 0.1;
                float3 pos1 = mul(r1, dirDist(time1 * pSpeed, DISTANCE));
                float3 pos2 = mul(r2, dirDist(time2 * pSpeed, DISTANCE));
                
                float seg = segment_distance_square(pos2.xy, pos1.xy, suv);
                circles_sq = min(circles_sq, seg);
            }
            
            float intensity = clamps(1.0 - sqrt(circles_sq) * 40.0);
            
            float3 buffA = pow(float3(intensity, intensity, intensity), float3(2.5, 1.8, 1.0));
            
            if (i == 0) 
            {
                currentHead = buffA;
            }
            
            float3 buffB = pow(buffA, float3(10.0, 10.0, 10.0));
            float decay = pow(0.0001, f * stepDelta);
            accumulatedTrail += buffB * decay * 1.2;
        }
        
        float3 col = currentHead + accumulatedTrail;
        
        float3 bgGlow = max(0.0, 1.0 - length(suv) * 2.0) * float3(0.1, 0.12, 0.3);
        col += bgGlow;
        
        float maxCol = max(max(col.r, col.g), col.b);
        float alpha = clamps(maxCol * 2.5);
        
        float progress = clamp((iTransitionProgress > 0.0 ? iTransitionProgress : 1.0) * 2.0, 0.0, 1.0);
        return float4(col * progress, alpha * progress);
    }
)";

const char* g_PassthroughShaderHLSL = R"(
    Texture2D g_texture : register(t0);
    SamplerState g_sampler : register(s0);
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

    PSInput VSMain(uint id : SV_VertexID) {
        PSInput output;
        float2 uv = float2((id << 1) & 2, id & 2);
        output.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
        output.uv = uv;
        return output;
    }

    float4 PSMain(PSInput input) : SV_TARGET {
        return g_texture.Sample(g_sampler, input.uv);
    }
)";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    const WCHAR szClassName[] = L"DirectPortProducerD3D12WindowClass";
    WNDCLASSEXW wcex = { sizeof(WNDCLASSEXW) };
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.lpszClassName = szClassName;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    wcex.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(DPD_ICON1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wcex);
    
    DWORD pid = GetCurrentProcessId();
    g_sharedTextureName = L"Local\\DirectPortTexture_" + std::to_wstring(pid);
    g_sharedFenceName = L"Local\\DirectPortFence_" + std::to_wstring(pid);
    
    // Placeholder title; dynamically populated after Manifest bounds to GPU Adapter
    WCHAR placeholderTitle[] = L"DirectPort Producer (D3D12) - Initializing...";

    RECT rc = { 0, 0, 1280, 720 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    g_hwnd = CreateWindowExW(0, szClassName, placeholderTitle, WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hInstance, nullptr);

    InitD3D12(g_hwnd);
    EnablePersistentAcrylic(g_hwnd);
    LoadAssets();
    InitializeSharing(1280, 720);

    ShowWindow(g_hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            RenderFrame();
        }
    }

    Cleanup();
    return static_cast<int>(msg.wParam);
}

void EnablePersistentAcrylic(HWND hwnd) {
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        auto setWCA = (pSetWindowCompositionAttribute)GetProcAddress(hUser, "SetWindowCompositionAttribute");
        if (setWCA) {
            // Derived cleanly from your inspiration code to force hardware blur
            ACCENT_POLICY policy = { ACCENT_ENABLE_BLURBEHIND, 0, 0, 0 };
            WINDOWCOMPOSITIONATTRIBDATA data = { 19 /* WCA_ACCENT_POLICY */, &policy, sizeof(policy) };
            setWCA(hwnd, &data);
        }
    }

    // Clip the DWM blur to rounded corners natively
    int corners = 2; /* DWMWCP_ROUNDSMALL */
    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corners, sizeof(corners));

    // Force Immersive Dark Mode so standard Title Bar is sleek black instead of opaque white
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));
}

void InitD3D12(HWND hwnd) {
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
    }

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        MessageBoxW(hwnd, L"Failed to create DXGI Factory.", L"Fatal Error", MB_ICONERROR);
        ExitProcess(1);
    }

    g_device.Reset();
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)))) {
        MessageBoxW(hwnd, L"Failed to create D3D12 Device.", L"Fatal Error", MB_ICONERROR);
        ExitProcess(1);
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue));

    RECT rc;
    GetClientRect(hwnd, &rc);

    // CRASH FIX: Removed the explicit DXGI_ALPHA_MODE_PREMULTIPLIED override. 
    // CreateSwapChainForHwnd rejects premultiplied alpha on standard Overlapped Windows.
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = rc.right - rc.left;
    swapChainDesc.Height = rc.bottom - rc.top;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    
    ComPtr<IDXGISwapChain1> swapChain;
    HRESULT hr = factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    
    // Explicit crash protection so we know if this fails rather than silently access violating
    if (FAILED(hr)) {
        LogHRESULT(L"CreateSwapChainForHwnd Failed", hr);
        MessageBoxW(hwnd, L"Failed to create D3D12 Swap Chain.", L"Fatal Error", MB_ICONERROR);
        ExitProcess(1);
    }

    swapChain.As(&g_swapChain);
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap));
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT n = 0; n < kFrameCount; n++) {
        g_swapChain->GetBuffer(n, IID_PPV_ARGS(&g_renderTargets[n]));
        g_device->CreateRenderTargetView(g_renderTargets[n].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += (1 * g_rtvDescriptorSize);
    }

    for (UINT n = 0; n < kFrameCount; n++) {
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[n]));
    }
    
    g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[g_frameIndex].Get(), nullptr, IID_PPV_ARGS(&g_commandList));
    g_commandList->Close();

    g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_renderFence));
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void LoadAssets() {
    D3D12_DESCRIPTOR_RANGE ranges[1] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER rootParameters[2] = {};
    rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[0].Descriptor = { 0, 0 }; 
    rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParameters[1].DescriptorTable = { 1, ranges }; 

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.NumStaticSamplers = 1;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> signature, error;
    D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    g_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&g_rootSignature));

    ComPtr<ID3DBlob> vertexShader, genPixelShader, ptPixelShader, passthroughVS;
    D3DCompile(g_GenerationShaderHLSL, strlen(g_GenerationShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vertexShader, &error);
    D3DCompile(g_GenerationShaderHLSL, strlen(g_GenerationShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &genPixelShader, &error);
    D3DCompile(g_PassthroughShaderHLSL, strlen(g_PassthroughShaderHLSL), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &passthroughVS, &error);
    D3DCompile(g_PassthroughShaderHLSL, strlen(g_PassthroughShaderHLSL), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ptPixelShader, &error);

    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizerDesc.DepthClipEnable = TRUE;

    D3D12_BLEND_DESC blendDesc = {};
    const D3D12_RENDER_TARGET_BLEND_DESC defaultRenderTargetBlendDesc = { FALSE,FALSE, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD, D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL, };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) blendDesc.RenderTarget[i] = defaultRenderTargetBlendDesc;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = g_rootSignature.Get();
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    
    psoDesc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
    psoDesc.PS = { genPixelShader->GetBufferPointer(), genPixelShader->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; 
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState));

    psoDesc.VS = { passthroughVS->GetBufferPointer(), passthroughVS->GetBufferSize() };
    psoDesc.PS = { ptPixelShader->GetBufferPointer(), ptPixelShader->GetBufferSize() };
    psoDesc.InputLayout = { nullptr, 0 }; 
    g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_passthroughPSO));

    D3D12_HEAP_PROPERTIES uploadHeapProps = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    
    // MEMORY RACE FIX: Ensures 256-byte alignment scales correctly with flight frames 
    bufferDesc.Width = ((sizeof(ConstantBuffer) + 255) & ~255) * kFrameCount;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    
    g_device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_constantBuffer));
    
    D3D12_RANGE readRange = {};
    g_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&g_pCbvDataBegin));
}

HRESULT InitializeSharing(UINT width, UINT height) {
    if (width == 0 || height == 0) return E_INVALIDARG;
    
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = 1;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_sharedRtvHeap));

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap));
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    
    D3D12_HEAP_PROPERTIES heapProps = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_CLEAR_VALUE optimizedClearValue = {};
    optimizedClearValue.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    optimizedClearValue.Color[0] = 0.0f;
    optimizedClearValue.Color[1] = 0.0f;
    optimizedClearValue.Color[2] = 0.0f;
    optimizedClearValue.Color[3] = 0.0f; 
    
    HRESULT hr = g_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc, D3D12_RESOURCE_STATE_COMMON, &optimizedClearValue, IID_PPV_ARGS(&g_sharedTexture));
    if (FAILED(hr)) { LogHRESULT(L"Sharing: CreateCommittedResource for texture FAILED", hr); return hr; }
    
    g_device->CreateRenderTargetView(g_sharedTexture.Get(), nullptr, g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart());
    g_device->CreateShaderResourceView(g_sharedTexture.Get(), nullptr, g_srvHeap->GetCPUDescriptorHandleForHeapStart());

    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) return E_FAIL;
    sa.lpSecurityDescriptor = sd;
    
    g_device->CreateSharedHandle(g_sharedTexture.Get(), &sa, GENERIC_ALL, g_sharedTextureName.c_str(), &g_sharedTextureHandle);
    g_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g_sharedFence));
    g_device->CreateSharedHandle(g_sharedFence.Get(), &sa, GENERIC_ALL, g_sharedFenceName.c_str(), &g_sharedFenceHandle);
    
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> adapter;
        LUID deviceLuid = g_device->GetAdapterLuid();
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (memcmp(&desc.AdapterLuid, &deviceLuid, sizeof(LUID)) == 0) {
                std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(GetCurrentProcessId());
                g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
                if (sd) LocalFree(sd);
                if (!g_hManifest) { LogHRESULT(L"CreateFileMappingW failed", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }

                g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
                g_pManifestView->width = width;
                g_pManifestView->height = height;
                g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
                g_pManifestView->adapterLuid = desc.AdapterLuid;
                wcscpy_s(g_pManifestView->textureName, g_sharedTextureName.c_str());
                wcscpy_s(g_pManifestView->fenceName, g_sharedFenceName.c_str());
                Log(L"Sharing session initialized successfully.");
                
                // --- DYNAMIC WINDOW TITLE ---
                // Updates directly after Broadcast Manifest successfully resolves the Adapter LUID
                WCHAR newTitle[512];
                wsprintfW(newTitle, L"DirectPort D3D12 Producer | PID: %lu | %ux%u | LUID: %08X | Tx: %s", 
                          GetCurrentProcessId(), width, height, desc.AdapterLuid.LowPart, g_sharedTextureName.c_str());
                SetWindowTextW(g_hwnd, newTitle);

                return S_OK;
            }
        }
    }
    if (sd) LocalFree(sd);
    return E_FAIL;
}

void ShutdownSharing() {
    if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
    if (g_hManifest) CloseHandle(g_hManifest);
    if (g_sharedFenceHandle) CloseHandle(g_sharedFenceHandle);
    if (g_sharedTextureHandle) CloseHandle(g_sharedTextureHandle);
    g_pManifestView = nullptr;
    g_hManifest = nullptr;
    g_sharedFenceHandle = nullptr;
    g_sharedTextureHandle = nullptr;
    g_sharedFence.Reset();
    g_sharedTexture.Reset();
    g_sharedRtvHeap.Reset();
    Log(L"Sharing session shut down.");
}

void UpdateConstantBuffer(float width, float height) {
    ConstantBuffer cb = {};
    cb.resolution[0] = width;
    cb.resolution[1] = height;
    
    auto currentTime = std::chrono::high_resolution_clock::now();
    cb.time = std::chrono::duration<float>(currentTime - gStartTime).count();
    
    cb.transitionProgress = 1.0f;
    cb.dots = 8.0f;     
    cb.orbScale = 1.0f; 
    
    UINT cbvOffset = g_frameIndex * 256; 
    memcpy(g_pCbvDataBegin + cbvOffset, &cb, sizeof(ConstantBuffer));
}

void PopulateCommandList() {
    g_commandAllocators[g_frameIndex]->Reset();
    g_commandList->Reset(g_commandAllocators[g_frameIndex].Get(), g_pipelineState.Get());

    g_commandList->SetGraphicsRootSignature(g_rootSignature.Get());
    g_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // --- PASS 1: Render scene to the Shared Texture (Fixed Size) ---
    D3D12_RESOURCE_DESC sharedDesc = g_sharedTexture->GetDesc();
    UpdateConstantBuffer((float)sharedDesc.Width, (float)sharedDesc.Height);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_sharedTexture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE sharedRtvHandle = g_sharedRtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_commandList->OMSetRenderTargets(1, &sharedRtvHandle, FALSE, nullptr);
    
    const float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_commandList->ClearRenderTargetView(sharedRtvHandle, clearColor, 0, nullptr);

    D3D12_VIEWPORT viewport = { 0.0f, 0.0f, (float)sharedDesc.Width, (float)sharedDesc.Height, 0.0f, 1.0f };
    D3D12_RECT scissorRect = { 0, 0, (LONG)sharedDesc.Width, (LONG)sharedDesc.Height };
    g_commandList->RSSetViewports(1, &viewport);
    g_commandList->RSSetScissorRects(1, &scissorRect);
    
    D3D12_GPU_VIRTUAL_ADDRESS cbvAddress = g_constantBuffer->GetGPUVirtualAddress() + (g_frameIndex * 256);
    g_commandList->SetGraphicsRootConstantBufferView(0, cbvAddress);
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // --- PASS 2: Blit the Shared Texture to the Window's Back Buffer for Preview ---
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = g_sharedTexture.Get();
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_commandList->ResourceBarrier(2, barriers);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += (g_frameIndex * g_rtvDescriptorSize);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    
    g_commandList->SetPipelineState(g_passthroughPSO.Get());
    ID3D12DescriptorHeap* ppHeaps[] = { g_srvHeap.Get() };
    g_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    g_commandList->SetGraphicsRootDescriptorTable(1, g_srvHeap->GetGPUDescriptorHandleForHeapStart());

    RECT clientRect;
    GetClientRect(g_hwnd, &clientRect);
    D3D12_VIEWPORT windowViewport = { 0.0f, 0.0f, (float)(clientRect.right - clientRect.left), (float)(clientRect.bottom - clientRect.top), 0.0f, 1.0f };
    D3D12_RECT windowScissorRect = { 0, 0, (LONG)windowViewport.Width, (LONG)windowViewport.Height };
    g_commandList->RSSetViewports(1, &windowViewport);
    g_commandList->RSSetScissorRects(1, &windowScissorRect);
    g_commandList->DrawInstanced(3, 1, 0, 0);

    // Transition resources back to their original states
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(2, barriers);
    
    g_commandList->Close();
}

void RenderFrame() {
    PopulateCommandList();

    ID3D12CommandList* ppCommandLists[] = { g_commandList.Get() };
    g_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
    
    if (g_sharedFence) {
        g_commandQueue->Signal(g_sharedFence.Get(), ++g_sharedFrameValue);
    }
    
    if (g_pManifestView) {
         g_pManifestView->frameValue = g_sharedFrameValue;
         WakeByAddressAll(&g_pManifestView->frameValue);
    }

    g_swapChain->Present(1, 0);
    MoveToNextFrame();
}

void MoveToNextFrame() {
    const UINT64 currentFenceValue = g_fenceValue;
    g_commandQueue->Signal(g_renderFence.Get(), currentFenceValue);
    g_renderFenceValues[g_frameIndex] = currentFenceValue;
    g_fenceValue++;
    
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    
    if (g_renderFence->GetCompletedValue() < g_renderFenceValues[g_frameIndex]) {
        g_renderFence->SetEventOnCompletion(g_renderFenceValues[g_frameIndex], g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, INFINITE);
    }
}

void WaitForGpuIdle() {
    g_commandQueue->Signal(g_renderFence.Get(), g_fenceValue);
    g_renderFence->SetEventOnCompletion(g_fenceValue, g_fenceEvent);
    WaitForSingleObject(g_fenceEvent, INFINITE);
    g_fenceValue++;
}

void OnResize(UINT width, UINT height) {
    if (!g_swapChain) return;

    WaitForGpuIdle();

    for (UINT i = 0; i < kFrameCount; i++) {
        g_renderTargets[i].Reset();
        g_renderFenceValues[i] = g_renderFenceValues[g_frameIndex];
    }
    
    HRESULT hr = g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        LogHRESULT(L"Swap chain resize failed", hr);
        return;
    }
    
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; i++) {
        g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i]));
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_rtvDescriptorSize;
    }
}

void Cleanup() {
    WaitForGpuIdle();
    ShutdownSharing();
    CloseHandle(g_fenceEvent);
    if(g_pCbvDataBegin) g_constantBuffer->Unmap(0, nullptr);
    g_pCbvDataBegin = nullptr;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        
        case WM_ENTERSIZEMOVE:
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            KillTimer(hwnd, 1);
            return 0;
        case WM_TIMER:
            if (wParam == 1) RenderFrame();
            return 0;

        case WM_SIZE:
            if (g_device && wParam != SIZE_MINIMIZED) {
                OnResize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}