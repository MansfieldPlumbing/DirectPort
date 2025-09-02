#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11_4.h> 
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <sddl.h> 
#include <string>
#include <atomic>
#include <vector>

#include <d3d12.h>
#pragma comment(lib, "d3d12.lib")
#include <d3dcompiler.h>
#include <shlwapi.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "dxguid.lib")

#define NAME_OBJ(o,n) if(o){ (o)->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(n), (n)); }

using Microsoft::WRL::ComPtr;

// --- Logging ---
void Log(const std::wstring& msg) { WCHAR b[512]; wsprintfW(b, L"[TegrityCam] %s\n", msg.c_str()); OutputDebugStringW(b); }
void LogHRESULT(const std::wstring& msg, HRESULT hr) { WCHAR b[512]; wsprintfW(b, L"[TegrityCam] %s - HRESULT: 0x%08X\n", msg.c_str(), hr); OutputDebugStringW(b); }

// --- Manifest Definition ---
struct BroadcastManifest {
    UINT64 frameValue;
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    LUID adapterLuid;
    WCHAR textureName[256];
    WCHAR fenceName[256];
};

// --- D3D11 Globals ---
HWND g_hWnd = nullptr;
ComPtr<IDXGISwapChain> g_pSwapChain;
ComPtr<ID3D11Device> g_pd3dDevice;
ComPtr<ID3D11Device5> g_pd3dDevice5;
ComPtr<ID3D11DeviceContext> g_pImmediateContext;
ComPtr<ID3D11DeviceContext4> g_pImmediateContext4;
ComPtr<ID3D11DeviceContext> g_pSharedCtx; // <-- DEFERRED CONTEXT
ComPtr<ID3D11RenderTargetView> g_pRenderTargetView;
ComPtr<ID3D11VertexShader> g_pVertexShader;
ComPtr<ID3D11PixelShader> g_pPixelShaderYUY2;
ComPtr<ID3D11PixelShader> g_pPixelShaderNV12;
ComPtr<ID3D11InputLayout> g_pVertexLayout;
ComPtr<ID3D11Buffer> g_pVertexBuffer;
ComPtr<ID3D11Buffer> g_pVertexBufferMirrored;
ComPtr<ID3D11SamplerState> g_pSamplerLinear;
ComPtr<ID3D11Buffer> g_pConstantBuffer;

// --- Triple-Buffered Camera Texture Globals ---
ComPtr<ID3D11Texture2D> g_pCameraTextureY[3];
ComPtr<ID3D11ShaderResourceView> g_pCameraTextureSRVY[3];
ComPtr<ID3D11Texture2D> g_pCameraTextureUV[3];
ComPtr<ID3D11ShaderResourceView> g_pCameraTextureSRVUV[3];
std::atomic<int> g_write_idx = 0;
std::atomic<int> g_latest_idx = 0;
std::atomic<int> g_render_idx = 0;

// --- Shared Resource Globals ---
static std::wstring                 gSharedTextureName;
static std::wstring                 gSharedFenceName;
static ComPtr<ID3D11Texture2D>        gSharedTex;
static ComPtr<ID3D11RenderTargetView> g_pSharedTextureRTV;
static ComPtr<IDXGIKeyedMutex>        gKM;
static HANDLE                       gSharedNTHandle = nullptr;
static ComPtr<ID3D11Fence>            gSharedFence;
static HANDLE                       gSharedFenceHandle = nullptr;
static UINT64                       gFrameValue = 0;

// Globals for resolution cycling
static std::vector<UINT>            g_sharedResolutions = { 128, 240, 360, 480, 720, 1080, 1440, 2160 };
static int                          g_currentResolutionIndex = 3; // Start at 480p
static UINT                         g_currentSharedSize = g_sharedResolutions[3];

// --- Manifest Globals ---
static HANDLE                       g_hManifest = nullptr;
static BroadcastManifest* g_pManifestView = nullptr;

struct ConstantBuffer { unsigned int videoWidth, videoHeight; float p1, p2; };

int g_currentCameraIndex = -1;
ComPtr<IMFSourceReader> g_pSourceReader;
long g_videoWidth = 0;
long g_videoHeight = 0;
GUID g_videoFormat = { 0 };

CRITICAL_SECTION g_critSec;
bool g_bFullscreen = false;
std::atomic<bool> g_bIsNewFrameReady = false;
std::atomic<bool> g_bMirror = false;

#define IDT_SINGLECLICK_TIMER 1

// --- Forward Declarations ---
HRESULT InitWindow(HINSTANCE hInstance, int nCmdShow);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
HRESULT InitDevice();
void Render();
HRESULT InternalCycleCamera();
void ToggleFullscreen();
void OnResize(UINT width, UINT height);
void ShutdownSharing();
HRESULT InitializeSharing(UINT width, UINT height);
void InternalChangeSharedResolution(BOOL bCycleUp);
std::wstring GetResolutionType(UINT width, UINT height);

// --- Extern "C" Interface ---
extern "C" {
    __declspec(dllexport) void CycleCameraSource();
    __declspec(dllexport) void CycleSharedResolution(BOOL bCycleUp);
    __declspec(dllexport) void ToggleMirrorStream();
}

void CycleCameraSource() { PostMessage(g_hWnd, WM_APP + 1, 0, 0); }
void CycleSharedResolution(BOOL bCycleUp) { PostMessage(g_hWnd, WM_APP + 2, bCycleUp, 0); }
void ToggleMirrorStream() { PostMessage(g_hWnd, WM_APP + 3, 0, 0); }


const char* g_strVertexShader = "struct VS_INPUT{float4 Pos:POSITION;float2 Tex:TEXCOORD0;};struct PS_INPUT{float4 Pos:SV_POSITION;float2 Tex:TEXCOORD0;};PS_INPUT VS(VS_INPUT i){return i;}";
const char* g_strPixelShaderYUY2 = "Texture2D t:register(t0);SamplerState s:register(s0);cbuffer cb:register(b0){uint w;uint h;};struct PS_INPUT{float4 P:SV_POSITION;float2 T:TEXCOORD0;};float4 PS(PS_INPUT i):SV_Target{float4 yuyv=t.Sample(s,i.T);float y;float u=yuyv.y;float v=yuyv.w;if(frac(i.T.x*(w/2.0f))>0.5f){y=yuyv.z;}else{y=yuyv.x;}float r=y+1.402f*(v-0.5f);float g=y-0.344f*(u-0.5f)-0.714f*(v-0.5f);float b=y+1.772f*(u-0.5f);return float4(r,g,b,1.0f);}";
const char* g_strPixelShaderNV12 = "Texture2D ty:register(t0);Texture2D tuv:register(t1);SamplerState s:register(s0);struct PS_INPUT{float4 P:SV_POSITION;float2 T:TEXCOORD0;};float4 PS(PS_INPUT i):SV_Target{float y=ty.Sample(s,i.T).r;float2 uv=tuv.Sample(s,i.T).rg;float r=y+1.402f*(uv.y-0.5f);float g=y-0.344f*(uv.x-0.5f)-0.714f*(uv.y-0.5f);float b=y+1.772f*(uv.x-0.5f);return float4(r,g,b,1.0f);}";

class CaptureManager : public IMFSourceReaderCallback
{
public:
    static HRESULT CreateInstance(CaptureManager** ppManager) { *ppManager = new (std::nothrow) CaptureManager(); return (*ppManager) ? S_OK : E_OUTOFMEMORY; }
    STDMETHODIMP QueryInterface(REFIID iid, void** ppv) { static const QITAB qit[] = { QITABENT(CaptureManager, IMFSourceReaderCallback), { 0 } }; return QISearch(this, qit, iid, ppv); }
    STDMETHODIMP_(ULONG) AddRef() { return InterlockedIncrement(&m_nRefCount); }
    STDMETHODIMP_(ULONG) Release() { ULONG uCount = InterlockedDecrement(&m_nRefCount); if (uCount == 0) delete this; return uCount; }

    STDMETHODIMP OnReadSample(HRESULT hrStatus, DWORD, DWORD, LONGLONG, IMFSample* pSample)
    {
        if (SUCCEEDED(hrStatus) && pSample)
        {
            ComPtr<IMFMediaBuffer> pBuffer;
            if (SUCCEEDED(pSample->ConvertToContiguousBuffer(&pBuffer)))
            {
                BYTE* pData = nullptr;
                if (SUCCEEDED(pBuffer->Lock(&pData, nullptr, nullptr)))
                {
                    EnterCriticalSection(&g_critSec);
                    if (g_pImmediateContext)
                    {
                        int currentWriteSet = g_write_idx.load();
                        
                        if (g_videoFormat == MFVideoFormat_YUY2 && g_pCameraTextureY[currentWriteSet])
                            g_pImmediateContext->UpdateSubresource(g_pCameraTextureY[currentWriteSet].Get(), 0, nullptr, pData, (g_videoWidth / 2) * 4, 0);
                        else if (g_videoFormat == MFVideoFormat_NV12 && g_pCameraTextureY[currentWriteSet] && g_pCameraTextureUV[currentWriteSet]) {
                            g_pImmediateContext->UpdateSubresource(g_pCameraTextureY[currentWriteSet].Get(), 0, nullptr, pData, g_videoWidth, 0);
                            g_pImmediateContext->UpdateSubresource(g_pCameraTextureUV[currentWriteSet].Get(), 0, nullptr, pData + g_videoWidth * g_videoHeight, g_videoWidth, 0);
                        }
                        
                        g_latest_idx.store(currentWriteSet);
                        int nextWriteIdx = (currentWriteSet + 1) % 3;
                        if(nextWriteIdx == g_render_idx.load())
                        {
                           nextWriteIdx = (nextWriteIdx + 1) % 3;
                        }
                        g_write_idx.store(nextWriteIdx);
                        
                        g_bIsNewFrameReady = true;
                    }
                    LeaveCriticalSection(&g_critSec);
                    pBuffer->Unlock();
                }
            }
        }
        EnterCriticalSection(&g_critSec);
        if (g_pSourceReader) g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
        LeaveCriticalSection(&g_critSec);
        return S_OK;
    }

    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) { return S_OK; }
    STDMETHODIMP OnFlush(DWORD) { return S_OK; }

private:
    CaptureManager() : m_nRefCount(1) {}
    virtual ~CaptureManager() {}
    long m_nRefCount;
};

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    DWORD pid = GetCurrentProcessId();
    gSharedTextureName = L"Global\\TegrityCamTexture_" + std::to_wstring(pid);
    gSharedFenceName = L"Global\\TegrityCamFence_" + std::to_wstring(pid);

    if (FAILED(InitWindow(hInstance, nCmdShow))) return 0;
    InitializeCriticalSection(&g_critSec);
    MFStartup(MF_VERSION);
    
    MSG msg = { 0 };

    if (SUCCEEDED(InitDevice()))
    {
        InternalCycleCamera();
        while (WM_QUIT != msg.message)
        {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
            else { Render(); }
        }
    }
    
    ShutdownSharing();
    MFShutdown();
    DeleteCriticalSection(&g_critSec);
    return (int)msg.wParam;
}

HRESULT InitWindow(HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEX wcex = { sizeof(WNDCLASSEX) };
    wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance;
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszClassName = L"D3D11CameraApp";
    if (!RegisterClassEx(&wcex)) return E_FAIL;
    g_hWnd = CreateWindow(L"D3D11CameraApp", L"D3D11 Camera (Click-Cycle Cam, Wheel-Cycle Res)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720, nullptr, nullptr, hInstance, nullptr);
    if (!g_hWnd) return E_FAIL;
    ShowWindow(g_hWnd, nCmdShow);
    return S_OK;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_DESTROY: PostQuitMessage(0); break;
    case WM_LBUTTONDOWN: SetTimer(hWnd, IDT_SINGLECLICK_TIMER, GetDoubleClickTime(), NULL); break;
    case WM_LBUTTONDBLCLK: KillTimer(hWnd, IDT_SINGLECLICK_TIMER); ToggleFullscreen(); break;
    case WM_TIMER: if (wParam == IDT_SINGLECLICK_TIMER) { KillTimer(hWnd, IDT_SINGLECLICK_TIMER); InternalCycleCamera(); } break;
    case WM_KEYDOWN: 
        if (wParam == VK_RETURN) { ToggleFullscreen(); }
        break;
    case WM_MOUSEWHEEL:
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            InternalChangeSharedResolution(delta > 0);
        }
        break;
    case WM_RBUTTONDOWN:
        g_bMirror = !g_bMirror;
        break;
    case WM_APP + 1: InternalCycleCamera(); break;
    case WM_APP + 2: InternalChangeSharedResolution((BOOL)wParam); break;
    case WM_APP + 3: g_bMirror = !g_bMirror; break;
    case WM_SIZE: if (g_pSwapChain && wParam != SIZE_MINIMIZED) { OnResize(LOWORD(lParam), HIWORD(lParam)); } break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

HRESULT InitDevice()
{
    RECT rc; GetClientRect(g_hWnd, &rc);
    UINT width = rc.right - rc.left, height = rc.bottom - rc.top;
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 3;
    sd.BufferDesc.Width = width; sd.BufferDesc.Height = height; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = g_hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pImmediateContext);
    if (FAILED(hr)) { LogHRESULT(L"D3D11CreateDeviceAndSwapChain FAILED.", hr); return hr; }
    
    // ---vvv--- Create Deferred Context ---vvv---
    g_pd3dDevice->CreateDeferredContext(0, &g_pSharedCtx);
    // ---^^^--- Create Deferred Context ---^^^---

    hr = g_pd3dDevice.As(&g_pd3dDevice5);
    if (FAILED(hr)) { LogHRESULT(L"Failed to query for ID3D11Device5.", hr); return hr; }
    hr = g_pImmediateContext.As(&g_pImmediateContext4);
    if (FAILED(hr)) { LogHRESULT(L"Failed to query for ID3D11DeviceContext4.", hr); return hr; }

    ComPtr<ID3D11Texture2D> pBackBuffer; hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)); if(FAILED(hr)) return hr;
    hr = g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), nullptr, &g_pRenderTargetView); if(FAILED(hr)) return hr;
    NAME_OBJ(g_pRenderTargetView.Get(), "BackbufferRTV");

    D3D11_VIEWPORT vp = { 0.f, 0.f, (FLOAT)width, (FLOAT)height, 0.f, 1.f }; g_pImmediateContext->RSSetViewports(1, &vp);
    ComPtr<ID3DBlob> pVSBlob, pPSBlobYUY2, pPSBlobNV12;
    D3DCompile(g_strVertexShader, strlen(g_strVertexShader), nullptr, nullptr, nullptr, "VS", "vs_4_0", 0, 0, &pVSBlob, nullptr);
    g_pd3dDevice->CreateVertexShader(pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), nullptr, &g_pVertexShader);
    D3D11_INPUT_ELEMENT_DESC layout[] = {{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    g_pd3dDevice->CreateInputLayout(layout, ARRAYSIZE(layout), pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), &g_pVertexLayout);
    D3DCompile(g_strPixelShaderYUY2, strlen(g_strPixelShaderYUY2), nullptr, nullptr, nullptr, "PS", "ps_4_0", 0, 0, &pPSBlobYUY2, nullptr);
    g_pd3dDevice->CreatePixelShader(pPSBlobYUY2->GetBufferPointer(), pPSBlobYUY2->GetBufferSize(), nullptr, &g_pPixelShaderYUY2);
    D3DCompile(g_strPixelShaderNV12, strlen(g_strPixelShaderNV12), nullptr, nullptr, nullptr, "PS", "ps_4_0", 0, 0, &pPSBlobNV12, nullptr);
    g_pd3dDevice->CreatePixelShader(pPSBlobNV12->GetBufferPointer(), pPSBlobNV12->GetBufferSize(), nullptr, &g_pPixelShaderNV12);
    struct SimpleVertex { float Pos[3]; float Tex[2]; }; SimpleVertex vertices[] = {{{-1,1,.5f},{0,0}},{{1,1,.5f},{1,0}},{{-1,-1,.5f},{0,1}},{{1,-1,.5f},{1,1}}};
    D3D11_SUBRESOURCE_DATA InitData = {vertices}; D3D11_BUFFER_DESC bd = {sizeof(vertices),D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER}; g_pd3dDevice->CreateBuffer(&bd,&InitData,&g_pVertexBuffer);
    SimpleVertex verticesMirrored[] = { {{-1,1,.5f},{1,0}},{{1,1,.5f},{0,0}},{{-1,-1,.5f},{1,1}},{{1,-1,.5f},{0,1}} };
    D3D11_SUBRESOURCE_DATA InitDataMirrored = { verticesMirrored }; g_pd3dDevice->CreateBuffer(&bd, &InitDataMirrored, &g_pVertexBufferMirrored);
    D3D11_SAMPLER_DESC sampDesc = {}; sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.MaxLOD = D3D11_FLOAT32_MAX; g_pd3dDevice->CreateSamplerState(&sampDesc,&g_pSamplerLinear);
    D3D11_BUFFER_DESC cbd = {sizeof(ConstantBuffer),D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE}; g_pd3dDevice->CreateBuffer(&cbd,nullptr,&g_pConstantBuffer);
    
    if (FAILED(InitializeSharing(g_currentSharedSize, g_currentSharedSize))) {
        Log(L"Failed to initialize sharing session. Will run as local viewer only.");
    }
    
    Log(L"InitDevice FINISHED successfully.");
    return S_OK;
}

void Render()
{
    // Modified to take a context pointer
    auto RenderScene = [&](ID3D11DeviceContext* pCtx, ID3D11RenderTargetView* rtv, UINT width, UINT height, int textureSet) {
        if (!pCtx || !rtv || !g_pCameraTextureSRVY[textureSet]) return;

        D3D11_VIEWPORT vp = { 0.f, 0.f, (FLOAT)width, (FLOAT)height, 0.f, 1.f };
        pCtx->RSSetViewports(1, &vp);
        pCtx->OMSetRenderTargets(1, &rtv, nullptr);
        
        if (rtv == g_pRenderTargetView.Get()) {
            float ClearColor[] = { 0.0f, 0.0f, 0.2f, 1.0f };
            pCtx->ClearRenderTargetView(rtv, ClearColor);
        }

        if (g_videoWidth > 0)
        {
            pCtx->IASetInputLayout(g_pVertexLayout.Get()); 
            UINT s = sizeof(float) * 5, o = 0; 
            ID3D11Buffer* pVB = g_bMirror ? g_pVertexBufferMirrored.Get() : g_pVertexBuffer.Get();
            pCtx->IASetVertexBuffers(0, 1, &pVB, &s, &o); 
            pCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP); 
            pCtx->VSSetShader(g_pVertexShader.Get(), nullptr, 0); 
            pCtx->PSSetSamplers(0, 1, g_pSamplerLinear.GetAddressOf());
            
            // NOTE: Map must be done on an immediate context, but since this is just updating
            // a constant buffer that is not shared, it's safe to do it here.
            D3D11_MAPPED_SUBRESOURCE mappedResource;
            if (SUCCEEDED(g_pImmediateContext->Map(g_pConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource))) { auto p = static_cast<ConstantBuffer*>(mappedResource.pData); p->videoWidth = g_videoWidth; p->videoHeight = g_videoHeight; g_pImmediateContext->Unmap(g_pConstantBuffer.Get(), 0); }
            pCtx->PSSetConstantBuffers(0, 1, g_pConstantBuffer.GetAddressOf());

            if (g_videoFormat == MFVideoFormat_YUY2) { pCtx->PSSetShader(g_pPixelShaderYUY2.Get(), nullptr, 0); pCtx->PSSetShaderResources(0, 1, g_pCameraTextureSRVY[textureSet].GetAddressOf()); }
            else if (g_videoFormat == MFVideoFormat_NV12) { pCtx->PSSetShader(g_pPixelShaderNV12.Get(), nullptr, 0); ID3D11ShaderResourceView* srvs[] = { g_pCameraTextureSRVY[textureSet].Get(), g_pCameraTextureSRVUV[textureSet].Get() }; pCtx->PSSetShaderResources(0, 2, srvs); }
            pCtx->Draw(4, 0);
        }
    };

    EnterCriticalSection(&g_critSec);

    if (g_bIsNewFrameReady.exchange(false)) {
        g_render_idx.store(g_latest_idx.load());
    }
    
    int stable_render_idx = g_render_idx.load();

    // --- Phase A (shared): record on deferred, execute on immediate ---
    if (gSharedTex && g_pSharedTextureRTV && gKM && g_pSharedCtx) { // Fixed typo here
        if (SUCCEEDED(gKM->AcquireSync(0, 16))) {
            // Record commands on the deferred context
            RenderScene(g_pSharedCtx.Get(), g_pSharedTextureRTV.Get(), g_currentSharedSize, g_currentSharedSize, stable_render_idx);

            // Unbind resources to keep the command list clean
            ID3D11RenderTargetView* nullRTV[] = { nullptr };
            g_pSharedCtx->OMSetRenderTargets(1, nullRTV, nullptr);
            
            // Finish the command list
            ComPtr<ID3D11CommandList> pCommandList;
            g_pSharedCtx->FinishCommandList(FALSE, &pCommandList);

            // Execute on the immediate context
            g_pImmediateContext->ExecuteCommandList(pCommandList.Get(), FALSE);

            // Update fence and manifest data
            gFrameValue++;
            if (g_pImmediateContext4 && gSharedFence)
                g_pImmediateContext4->Signal(gSharedFence.Get(), gFrameValue);
            if (g_pManifestView)
                InterlockedExchange64((volatile LONGLONG*)&g_pManifestView->frameValue, gFrameValue);
            
            // Unbind, Flush, and THEN Release. This is the correct, safe order.
            ID3D11RenderTargetView* nullRTVs[] = { nullptr };
            g_pImmediateContext->OMSetRenderTargets(1, nullRTVs, nullptr);
            g_pImmediateContext->Flush(); 
            gKM->ReleaseSync(1);
        }
    }
    
    // --- Hard reset before the window pass ---
    g_pImmediateContext->ClearState();

    // --- Phase B (local window) ---
    RECT rc; GetClientRect(g_hWnd, &rc);
    RenderScene(g_pImmediateContext.Get(), g_pRenderTargetView.Get(), rc.right - rc.left, rc.bottom - rc.top, stable_render_idx);

    LeaveCriticalSection(&g_critSec);
    g_pSwapChain->Present(1, 0);
}

std::wstring GetResolutionType(UINT width, UINT height) {
    if (height >= 2160) return L"4K (UHD)";
    if (height >= 1440) return L"QHD";
    if (height >= 1080) return L"FHD";
    if (height >= 720) return L"HD";
    if (height >= 480) return L"SD";
    return L"";
}

void InternalChangeSharedResolution(BOOL bCycleUp)
{
    if (bCycleUp) {
        g_currentResolutionIndex = (g_currentResolutionIndex + 1) % g_sharedResolutions.size();
    } else {
        g_currentResolutionIndex--;
        if (g_currentResolutionIndex < 0) {
            g_currentResolutionIndex = g_sharedResolutions.size() - 1;
        }
    }
    
    UINT newSize = g_sharedResolutions[g_currentResolutionIndex];

    if (g_currentSharedSize == newSize) return;

    Log(L"Changing shared resolution to " + std::to_wstring(newSize) + L"x" + std::to_wstring(newSize));

    std::wstring resType = GetResolutionType(newSize, newSize);
    std::wstring title = L"D3D11 Camera (Shared: " + std::to_wstring(newSize) + L"x" + std::to_wstring(newSize) + L" " + resType + L")";
    SetWindowTextW(g_hWnd, title.c_str());

    EnterCriticalSection(&g_critSec);
    ShutdownSharing();
    g_currentSharedSize = newSize;
    if (FAILED(InitializeSharing(g_currentSharedSize, g_currentSharedSize))) {
        Log(L"Failed to re-initialize sharing session. Will run as local viewer only.");
    }
    LeaveCriticalSection(&g_critSec);
}

void ToggleFullscreen() { if (g_pSwapChain) { g_bFullscreen = !g_bFullscreen; g_pSwapChain->SetFullscreenState(g_bFullscreen, nullptr); } }
void OnResize(UINT width, UINT height)
{
    if (g_pRenderTargetView){
        g_pImmediateContext->OMSetRenderTargets(0,nullptr,nullptr); g_pRenderTargetView.Reset();
        if(SUCCEEDED(g_pSwapChain->ResizeBuffers(3,width,height,DXGI_FORMAT_R8G8B8A8_UNORM,0))){
            ComPtr<ID3D11Texture2D>pBackBuffer; g_pSwapChain->GetBuffer(0,IID_PPV_ARGS(&pBackBuffer)); 
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(),nullptr,&g_pRenderTargetView);
            NAME_OBJ(g_pRenderTargetView.Get(), "BackbufferRTV");
        }
    }
}

HRESULT InternalCycleCamera()
{
    EnterCriticalSection(&g_critSec);
    if (g_pSourceReader) { g_pSourceReader.Reset(); }
    g_videoWidth = 0; g_videoHeight = 0; 
    for(int i = 0; i < 3; ++i) {
        g_pCameraTextureY[i].Reset(); g_pCameraTextureSRVY[i].Reset();
        g_pCameraTextureUV[i].Reset(); g_pCameraTextureSRVUV[i].Reset();
    }
    g_write_idx = 0; g_latest_idx = 0; g_render_idx = 0;
    g_bIsNewFrameReady = false;
    LeaveCriticalSection(&g_critSec);

    ComPtr<IMFAttributes> pAttributes;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1); if(FAILED(hr)) return hr;
    hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID); if(FAILED(hr)) return hr;
    
    UINT32 count = 0;
    IMFActivate** devices = nullptr;
    hr = MFEnumDeviceSources(pAttributes.Get(), &devices, &count);
    if (FAILED(hr) || count == 0) { if(devices) CoTaskMemFree(devices); Log(L"No video capture devices found."); return E_FAIL; }

    g_currentCameraIndex = (g_currentCameraIndex + 1) % count;

    ComPtr<IMFMediaSource> pSource;
    hr = devices[g_currentCameraIndex]->ActivateObject(IID_PPV_ARGS(&pSource));
    
    for (UINT32 i = 0; i < count; i++) { devices[i]->Release(); }
    CoTaskMemFree(devices);
    if (FAILED(hr)) return hr;

    ComPtr<IMFAttributes> pReaderAttributes;
    hr = MFCreateAttributes(&pReaderAttributes, 1); if(FAILED(hr)) return hr;
    ComPtr<CaptureManager> pCallback;
    CaptureManager::CreateInstance(&pCallback);
    hr = pReaderAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, pCallback.Get()); if(FAILED(hr)) return hr;

    EnterCriticalSection(&g_critSec);
    hr = MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttributes.Get(), &g_pSourceReader);
    LeaveCriticalSection(&g_critSec);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> pType;
    for (DWORD i = 0; SUCCEEDED(g_pSourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType)); ++i) {
        GUID subtype; pType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12 || subtype == MFVideoFormat_YUY2) {
            g_videoFormat = subtype; hr = g_pSourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType.Get()); break;
        } pType.Reset();
    } if (FAILED(hr)) return hr;

    ComPtr<IMFMediaType> pCurrentType;
    hr = g_pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType); if(FAILED(hr)) return hr;
    UINT32 width, height;
    MFGetAttributeSize(pCurrentType.Get(), MF_MT_FRAME_SIZE, &width, &height);

    EnterCriticalSection(&g_critSec);
    g_videoWidth = width; g_videoHeight = height;
    
    for(int i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Height = g_videoHeight; desc.MipLevels = 1; desc.ArraySize = 1; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        
        if (g_videoFormat == MFVideoFormat_YUY2) {
            desc.Width = g_videoWidth / 2;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        } else {
            desc.Width = g_videoWidth;
            desc.Format = DXGI_FORMAT_R8_UNORM;
        }
        
        g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_pCameraTextureY[i]);
        ComPtr<ID3D11RenderTargetView> rtvY;
        if (SUCCEEDED(g_pd3dDevice->CreateRenderTargetView(g_pCameraTextureY[i].Get(), nullptr, &rtvY))) {
            const float blue[] = { 0.0f, 0.0f, 0.2f, 1.0f };
            g_pImmediateContext->ClearRenderTargetView(rtvY.Get(), blue);
        }
        g_pd3dDevice->CreateShaderResourceView(g_pCameraTextureY[i].Get(), nullptr, &g_pCameraTextureSRVY[i]);

        if (g_videoFormat == MFVideoFormat_NV12) {
            desc.Width = g_videoWidth / 2; desc.Height = g_videoHeight / 2; desc.Format = DXGI_FORMAT_R8G8_UNORM;
            g_pd3dDevice->CreateTexture2D(&desc, nullptr, &g_pCameraTextureUV[i]);
            ComPtr<ID3D11RenderTargetView> rtvUV;
            if (SUCCEEDED(g_pd3dDevice->CreateRenderTargetView(g_pCameraTextureUV[i].Get(), nullptr, &rtvUV))) {
                const float neutralChroma[] = { 0.5f, 0.5f, 0.0f, 1.0f };
                g_pImmediateContext->ClearRenderTargetView(rtvUV.Get(), neutralChroma);
            }
            g_pd3dDevice->CreateShaderResourceView(g_pCameraTextureUV[i].Get(), nullptr, &g_pCameraTextureSRVUV[i]);
        }
    }
    
    hr = g_pSourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    LeaveCriticalSection(&g_critSec);
    return hr;
}

void ShutdownSharing() {
    if (g_pManifestView) { UnmapViewOfFile(g_pManifestView); g_pManifestView = nullptr; }
    if (g_hManifest) { CloseHandle(g_hManifest); g_hManifest = nullptr; }
    if (gSharedFenceHandle) { CloseHandle(gSharedFenceHandle); gSharedFenceHandle = nullptr; }
    if (gSharedNTHandle) { CloseHandle(gSharedNTHandle); gSharedNTHandle = nullptr; }
    gSharedFence.Reset();
    gKM.Reset();
    g_pSharedTextureRTV.Reset();
    gSharedTex.Reset();
    g_pSharedCtx.Reset();
    Log(L"Sharing session shut down.");
}

HRESULT InitializeSharing(UINT width, UINT height) {
    if (width == 0 || height == 0) return E_INVALIDARG;
    
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width; td.Height = height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    HRESULT hr = g_pd3dDevice->CreateTexture2D(&td, nullptr, &gSharedTex);
    if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: CreateTexture2D FAILED.", hr); return hr; }

    hr = g_pd3dDevice->CreateRenderTargetView(gSharedTex.Get(), nullptr, &g_pSharedTextureRTV);
    if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: CreateRenderTargetView FAILED.", hr); return hr; }
    NAME_OBJ(g_pSharedTextureRTV.Get(), "SharedRTV");
    
    hr = gSharedTex.As(&gKM);
    if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: Failed to get KeyedMutex.", hr); return hr; }

    if (g_pImmediateContext && g_pSharedTextureRTV && gKM) {
        if (SUCCEEDED(gKM->AcquireSync(0, INFINITE))) {
            const float noSignalColor[] = { 0.0f, 0.0f, 0.2f, 1.0f };
            g_pImmediateContext->ClearRenderTargetView(g_pSharedTextureRTV.Get(), noSignalColor);
            
            g_pImmediateContext->Flush();
            gKM->ReleaseSync(1);
        }
    }

    ComPtr<IDXGIResource1> r1;
    hr = gSharedTex.As(&r1);
    if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: Failed to query IDXGIResource1.", hr); return hr; }
    
    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &pSD, NULL)){
        SECURITY_ATTRIBUTES sa{ sizeof(sa), pSD, FALSE };
        hr = r1->CreateSharedHandle(&sa, GENERIC_ALL, gSharedTextureName.c_str(), &gSharedNTHandle);
        LocalFree(pSD);
        if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: CreateSharedHandle for texture FAILED.", hr); return hr; }
    } else { return E_FAIL; }

    hr = g_pd3dDevice5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gSharedFence));
    if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: CreateFence FAILED.", hr); return hr; }

    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &pSD, NULL)) {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), pSD, FALSE };
        hr = gSharedFence->CreateSharedHandle(&sa, GENERIC_ALL, gSharedFenceName.c_str(), &gSharedFenceHandle);
        LocalFree(pSD);
        if (FAILED(hr)) { LogHRESULT(L"InitializeSharing: CreateSharedHandle for fence FAILED.", hr); return hr; }
    } else { return E_FAIL; }

    std::wstring manifestName = L"TegrityCam_Manifest_" + std::to_wstring(GetCurrentProcessId());
    
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &pSD, NULL)) { return E_FAIL; }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };
    
    g_hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    LocalFree(pSD);
    if (g_hManifest == NULL) { LogHRESULT(L"CreateFileMappingW failed", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }

    g_pManifestView = (BroadcastManifest*)MapViewOfFile(g_hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (g_pManifestView == nullptr) { CloseHandle(g_hManifest); g_hManifest = nullptr; LogHRESULT(L"MapViewOfFile failed", HRESULT_FROM_WIN32(GetLastError())); return E_FAIL; }
    
    LUID adapterLuid = {};
    ComPtr<IDXGIDevice> dxgiDevice;
    if (SUCCEEDED(g_pd3dDevice.As(&dxgiDevice))) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC desc;
            if (SUCCEEDED(adapter->GetDesc(&desc))) { adapterLuid = desc.AdapterLuid; }
        }
    }
    
    ZeroMemory(g_pManifestView, sizeof(BroadcastManifest));
    g_pManifestView->width = width;
    g_pManifestView->height = height;
    g_pManifestView->format = DXGI_FORMAT_B8G8R8A8_UNORM;
    g_pManifestView->adapterLuid = adapterLuid;
    wcscpy_s(g_pManifestView->textureName, _countof(g_pManifestView->textureName), gSharedTextureName.c_str());
    wcscpy_s(g_pManifestView->fenceName, _countof(g_pManifestView->fenceName), gSharedFenceName.c_str());
    
    Log(L"New sharing session initialized successfully.");
    return S_OK;
}