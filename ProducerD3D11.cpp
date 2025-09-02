#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <wrl.h>
#include <sddl.h>
#include <string>
#include <chrono>
#include <vector>

#include <d3d12.h>
#pragma comment(lib, "d3d12.lib")

#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"advapi32.lib")

using Microsoft::WRL::ComPtr;

void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][Producer] %s\n", GetCurrentProcessId(), msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[PID:%lu][Producer] %s - HRESULT: 0x%08X\n", GetCurrentProcessId(), msg.c_str(), hr); OutputDebugStringW(b); }

static const UINT RENDER_W = 1280;
static const UINT RENDER_H = 720;
static const float BAR_BASE_WIDTH_NDC = 1.0f;
static const float BAR_ASPECT_RATIO = 16.0f / 10.0f;
static const float BAR_HEIGHT_NDC = BAR_BASE_WIDTH_NDC / BAR_ASPECT_RATIO * ((float)RENDER_W / RENDER_H);

struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};
struct ConstantBuffer{ float bar_rect[4]; float resolution[2]; float padding[2]; };

static ComPtr<ID3D11Device>           gDev;
static ComPtr<ID3D11Device5>          gDev5;
static ComPtr<ID3D11DeviceContext>    gCtx;
static ComPtr<ID3D11DeviceContext4>   gCtx4;
static ComPtr<IDXGISwapChain>         gSwap;
static ComPtr<ID3D11RenderTargetView> gWindowRTV;
static ComPtr<ID3D11Buffer>           gConstantBuffer;
static ComPtr<ID3D11SamplerState>     gSamplerState;
static ComPtr<ID3D11VertexShader>     gVertexShaderMain;
static ComPtr<ID3D11PixelShader>      gPixelShaderMain;
static ComPtr<ID3D11Texture2D>        gPrivateTex;
static ComPtr<ID3D11RenderTargetView> gPrivateTexRTV;

static std::wstring                 gSharedTextureName, gSharedFenceName;
static ComPtr<ID3D11Texture2D>        gSharedTex;
static HANDLE                       gSharedNTHandle = nullptr;
static ComPtr<ID3D11Fence>            gSharedFence;
static HANDLE                       gSharedFenceHandle = nullptr;
static UINT64                       gFrameValue = 0;

static HANDLE                       g_hManifest = nullptr;
static BroadcastManifest*           g_pManifestView = nullptr;

static float gBarPosX = 0.0f, gBarPosY = 0.0f;
static float gBarVelX = 0.2f, gBarVelY = 0.3f;
static auto gLastFrameTime = std::chrono::high_resolution_clock::now();

const char* g_VertexShaderMainHLSL = R"( struct VOut { float4 pos : SV_Position; }; VOut main(uint vid : SV_VertexID) { float2 uv = float2((vid << 1) & 2, vid & 2); VOut o; o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1); return o; } )";
const char* g_PixelShaderMainHLSL = R"( cbuffer Constants : register(b0) { float4 bar_rect; float2 resolution; }; float3 SmpteBar(float u) { if (u < 1.0/7.0) return float3(0.75, 0.75, 0.75); if (u < 2.0/7.0) return float3(0.75, 0.75, 0.00); if (u < 3.0/7.0) return float3(0.00, 0.75, 0.75); if (u < 4.0/7.0) return float3(0.00, 0.75, 0.00); if (u < 5.0/7.0) return float3(0.75, 0.00, 0.75); if (u < 6.0/7.0) return float3(0.75, 0.00, 0.00); return float3(0.00, 0.00, 0.75); } float4 main(float4 pos : SV_Position) : SV_Target { float2 ndc = pos.xy / resolution.xy * float2(2.0, -2.0) + float2(-1.0, 1.0); float3 bg = lerp(float3(0.01, 0.01, 0.03), float3(0.03, 0.02, 0.06), pos.y / resolution.y); bg *= (1.0 - 0.55 * smoothstep(0.55, 1.10, length(ndc))); float xL = bar_rect.x; float yT = bar_rect.y; float w = bar_rect.z; float h = bar_rect.w; float xR = xL + w; float yB = yT - h; if (ndc.x >= xL && ndc.x <= xR && ndc.y <= yT && ndc.y >= yB) { float u = (ndc.x - xL) / w; float v = (yT - ndc.y) / h; if (v > 0.75) return float4(0.0, 0.0, 0.0, 1.0); return float4(SmpteBar(u), 1.0); } return float4(bg, 1.0); } )";

static HRESULT CreateDevice(HWND hwnd);
static HRESULT CreateSharedTexture();
static HRESULT CreateSharedFence();
static HRESULT CreateBroadcastManifest();
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_DESTROY) PostQuitMessage(0); return DefWindowProcW(h, m, w, l); }
static void UpdateAnimation(float dt);
static void DrawMainScene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv);

void RenderFrame() {
    UpdateAnimation(std::chrono::duration<float>(std::chrono::high_resolution_clock::now() - gLastFrameTime).count());
    gLastFrameTime = std::chrono::high_resolution_clock::now();

    DrawMainScene(gCtx.Get(), gPrivateTexRTV.Get());

    // --- FENCE SYNCHRONIZATION LOGIC ---
    // 1. Copy the private texture to the shared one. This command is enqueued on the GPU.
    gCtx->CopyResource(gSharedTex.Get(), gPrivateTex.Get());

    // 2. Increment the frame counter.
    gFrameValue++;

    // 3. Enqueue a command for the GPU to signal the fence with the new frame value once the copy is complete.
    gCtx4->Signal(gSharedFence.Get(), gFrameValue);

    // 4. Update the manifest on the CPU so consumers can see the new target frame value.
    if (g_pManifestView) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&g_pManifestView->frameValue), gFrameValue);
    }
    // --- END OF FENCE LOGIC ---

    DrawMainScene(gCtx.Get(), gWindowRTV.Get()); 

    gSwap->Present(1, 0);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    Log(L"Producer START.");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.lpszClassName = L"GenNTWnd"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW); RegisterClassW(&wc);
    DWORD pid = GetCurrentProcessId(); 
    gSharedTextureName = L"Global\\TegrityCamTexture_" + std::to_wstring(pid);
    gSharedFenceName = L"Global\\TegrityCamFence_" + std::to_wstring(pid);
    std::wstring title = L"Producer (PID: " + std::to_wstring(pid) + L")";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, RENDER_W + 16, RENDER_H + 39, nullptr, nullptr, hi, nullptr);
    if (!hwnd) { Log(L"CreateWindowExW FAILED."); return 1; }

    if (FAILED(CreateDevice(hwnd))) { MessageBoxW(NULL, L"Failed to create DirectX device.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateSharedTexture())) { MessageBoxW(NULL, L"Failed to create shared texture.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateSharedFence())) { MessageBoxW(NULL, L"Failed to create shared fence.", L"Startup Error", MB_OK); return 2; }
    if (FAILED(CreateBroadcastManifest())) { MessageBoxW(NULL, L"Failed to create broadcast manifest.", L"Startup Error", MB_OK); return 2; }
    
    Log(L"Initialization successful. Entering render loop.");
    MSG msg{};
    while (msg.message != WM_QUIT) { if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } else { RenderFrame(); } }
    
    if (g_pManifestView) UnmapViewOfFile(g_pManifestView);
    if (g_hManifest) CloseHandle(g_hManifest);
    if (gSharedNTHandle) CloseHandle(gSharedNTHandle);
    if (gSharedFenceHandle) CloseHandle(gSharedFenceHandle);
    CoUninitialize();
    Log(L"Application shutting down.");
    return (int)msg.wParam;
}

HRESULT CreateDevice(HWND hwnd) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &gSwap, &gDev, nullptr, &gCtx);
    if (FAILED(hr)) { LogHRESULT(L"D3D11CreateDeviceAndSwapChain FAILED.", hr); return hr; }
    hr = gDev.As(&gDev5); if (FAILED(hr)) { LogHRESULT(L"Failed to query for ID3D11Device5.", hr); return hr; }
    hr = gCtx.As(&gCtx4); if (FAILED(hr)) { LogHRESULT(L"Failed to query for ID3D11DeviceContext4.", hr); return hr; }
    ComPtr<ID3D11Texture2D> backBuffer;
    gSwap->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    gDev->CreateRenderTargetView(backBuffer.Get(), nullptr, &gWindowRTV);
    D3D11_TEXTURE2D_DESC privateDesc = {};
    privateDesc.Width=RENDER_W; privateDesc.Height=RENDER_H; privateDesc.MipLevels=1; privateDesc.ArraySize=1;
    privateDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM; privateDesc.SampleDesc.Count=1; privateDesc.Usage=D3D11_USAGE_DEFAULT;
    privateDesc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    hr = gDev->CreateTexture2D(&privateDesc,nullptr,&gPrivateTex);
    if (FAILED(hr)) { LogHRESULT(L"Failed to create private texture.", hr); return hr; }
    hr = gDev->CreateRenderTargetView(gPrivateTex.Get(), nullptr, &gPrivateTexRTV);
    if (FAILED(hr)) { LogHRESULT(L"Failed to create private RTV.", hr); return hr; }
    ComPtr<ID3DBlob> vsMain, psMain, err;
    D3DCompile(g_VertexShaderMainHLSL, strlen(g_VertexShaderMainHLSL), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsMain, &err);
    D3DCompile(g_PixelShaderMainHLSL, strlen(g_PixelShaderMainHLSL), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &psMain, &err);
    gDev->CreateVertexShader(vsMain->GetBufferPointer(), vsMain->GetBufferSize(), nullptr, &gVertexShaderMain);
    gDev->CreatePixelShader(psMain->GetBufferPointer(), psMain->GetBufferSize(), nullptr, &gPixelShaderMain);
    D3D11_BUFFER_DESC cbDesc = {}; cbDesc.ByteWidth = sizeof(ConstantBuffer); cbDesc.Usage = D3D11_USAGE_DYNAMIC; cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    gDev->CreateBuffer(&cbDesc, nullptr, &gConstantBuffer);
    D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER; sampDesc.MinLOD = 0; sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    gDev->CreateSamplerState(&sampDesc, &gSamplerState);
    Log(L"CreateDevice FINISHED successfully.");
    return S_OK;
}

HRESULT CreateSharedTexture() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = RENDER_W;
    td.Height = RENDER_H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    // This is the corrected line, adding the required base SHARED flag.
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    
    HRESULT hr = gDev->CreateTexture2D(&td, nullptr, &gSharedTex);
    if (FAILED(hr)) { LogHRESULT(L"CreateTexture2D for shared texture FAILED.", hr); return hr; }
    
    ComPtr<IDXGIResource1> r1;
    hr = gSharedTex.As(&r1);
    if (FAILED(hr)) { LogHRESULT(L"Failed to query IDXGIResource1 for texture.", hr); return hr; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)){
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        hr = r1->CreateSharedHandle(&sa, GENERIC_ALL, gSharedTextureName.c_str(), &gSharedNTHandle);
        LocalFree(sd);
        if (FAILED(hr)) { LogHRESULT(L"CreateSharedHandle FAILED for texture.", hr); return hr; }
    } else { return E_FAIL; }
    return S_OK;
}

HRESULT CreateSharedFence() {
    if (!gDev5) return E_NOINTERFACE;
    HRESULT hr = gDev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gSharedFence));
    if (FAILED(hr)) { LogHRESULT(L"CreateFence FAILED.", hr); return hr; }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
        hr = gSharedFence->CreateSharedHandle(&sa, GENERIC_ALL, gSharedFenceName.c_str(), &gSharedFenceHandle);
        LocalFree(sd);
        if (FAILED(hr)) { LogHRESULT(L"CreateSharedHandle FAILED for fence.", hr); return hr; }
    } else { return E_FAIL; }
    return S_OK;
}

HRESULT CreateBroadcastManifest() {
    LUID adapterLuid = {};
    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(gDev.As(&dxgiDevice))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) { adapterLuid = desc.AdapterLuid; }
        }
    }
    std::wstring manifestName = L"TegrityCam_Manifest_" + std::to_wstring(GetCurrentProcessId());
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sd, NULL)) { return E_FAIL; }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), sd, FALSE };
    g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(sd);
    if (g_hManifest == NULL) { LogHRESULT(L"CreateFileMappingW FAILED.", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }
    g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (g_pManifestView == nullptr) { LogHRESULT(L"MapViewOfFile FAILED.", HRESULT_FROM_WIN32(GetLastError())); CloseHandle(g_hManifest); g_hManifest = nullptr; return E_FAIL; }
    
    ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
    g_pManifestView->width = RENDER_W;
    g_pManifestView->height = RENDER_H;
    g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestView->adapterLuid = adapterLuid;
    wcscpy_s(g_pManifestView->textureName, _countof(g_pManifestView->textureName), gSharedTextureName.c_str());
    wcscpy_s(g_pManifestView->fenceName, _countof(g_pManifestView->fenceName), gSharedFenceName.c_str());
    return S_OK;
}

void UpdateAnimation(float dt) {
    gBarPosX += gBarVelX * dt; gBarPosY += gBarVelY * dt;
    if (abs(gBarPosX) > (1.0f - BAR_BASE_WIDTH_NDC / 2.f)) { gBarVelX *= -1.0f; gBarPosX = (1.0f - BAR_BASE_WIDTH_NDC / 2.f) * (gBarPosX > 0 ? 1 : -1); }
    if (abs(gBarPosY) > (1.0f - BAR_HEIGHT_NDC / 2.f)) { gBarVelY *= -1.0f; gBarPosY = (1.0f - BAR_HEIGHT_NDC / 2.f) * (gBarPosY > 0 ? 1 : -1); }
}

void DrawMainScene(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv) {
    D3D11_VIEWPORT vp = { 0, 0, (float)RENDER_W, (float)RENDER_H, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(ctx->Map(gConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        ConstantBuffer cb;
        cb.resolution[0] = RENDER_W; cb.resolution[1] = RENDER_H;
        cb.bar_rect[0] = gBarPosX - BAR_BASE_WIDTH_NDC / 2.f; cb.bar_rect[1] = gBarPosY + BAR_HEIGHT_NDC / 2.f; cb.bar_rect[2] = BAR_BASE_WIDTH_NDC; cb.bar_rect[3] = BAR_HEIGHT_NDC;
        memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(gConstantBuffer.Get(), 0);
    }
    ctx->VSSetShader(gVertexShaderMain.Get(), nullptr, 0);
    ctx->PSSetShader(gPixelShaderMain.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, gConstantBuffer.GetAddressOf());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->Draw(3, 0);
}