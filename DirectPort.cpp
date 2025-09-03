#include "DirectPort.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <tlhelp32.h>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace Microsoft::WRL;
using namespace DirectPort;

namespace { 
    struct BroadcastManifest {
        UINT64 frameValue; UINT width; UINT height; DXGI_FORMAT format;
        LUID adapterLuid; WCHAR textureName[256]; WCHAR fenceName[256];
    };

    HANDLE GetHandleFromName_D3D12(const WCHAR* name) {
        ComPtr<ID3D12Device> d3d12Device;
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12Device)))) return NULL;
        HANDLE handle = nullptr;
        d3d12Device->OpenSharedHandleByName(name, GENERIC_ALL, &handle);
        return handle;
    }
    
    const char* g_BlitShader = R"(
        Texture2D g_texture : register(t0);
        SamplerState g_sampler : register(s0);
        struct VS_OUTPUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
        
        VS_OUTPUT VSMain(uint id : SV_VertexID) {
            VS_OUTPUT output;
            float2 uv = float2((id << 1) & 2, id & 2);
            output.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1);
            output.uv = uv;
            return output;
        }

        float4 PSMain(VS_OUTPUT input) : SV_TARGET {
            return g_texture.Sample(g_sampler, input.uv);
        }
    )";

    LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_CLOSE) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
}

class DirectPort::ProducerImpl {
public:
    DWORD pid;
    UINT width = 0, height = 0;
    HANDLE hManifest = nullptr, hProcess = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    ComPtr<ID3D11Texture2D> sharedTexture;
    ComPtr<ID3D11ShaderResourceView> sharedSRV;
    ComPtr<ID3D11Fence> sharedFence;
    UINT64 lastSeenFrame = 0;
    ComPtr<ID3D11Device5> d3d11Device5;
    ComPtr<ID3D11DeviceContext4> d3d11Context4;
    ~ProducerImpl() {
        if (pManifestView) UnmapViewOfFile(pManifestView);
        if (hManifest) CloseHandle(hManifest);
        if (hProcess) CloseHandle(hProcess);
    }
};

class DirectPort::ConsumerImpl {
public:
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11Device1> d3d11Device1;
    ComPtr<ID3D11Device5> d3d11Device5;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    ComPtr<ID3D11DeviceContext4> d3d11Context4;
    LUID adapterLuid = {};

    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11SamplerState> sampler;
    ~ConsumerImpl() {
        if (hwnd) DestroyWindow(hwnd);
    }
};

// --- Consumer Implementation ---

Consumer::Consumer(const std::wstring& title, int width, int height) : pImpl(std::make_unique<ConsumerImpl>()) {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0, 0, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"DirectPortConsumer", NULL };
    RegisterClassExW(&wc);
    pImpl->hwnd = CreateWindowW(wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, 100, 100, width, height, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2; scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = pImpl->hwnd; scd.SampleDesc.Count = 1; scd.Windowed = TRUE; scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &scd, &pImpl->swapChain, &pImpl->d3d11Device, nullptr, &pImpl->d3d11Context);
    if (FAILED(hr)) throw std::runtime_error("Consumer failed to create D3D11 device and swapchain.");
    
    pImpl->d3d11Device.As(&pImpl->d3d11Device1);
    pImpl->d3d11Device.As(&pImpl->d3d11Device5);
    pImpl->d3d11Context.As(&pImpl->d3d11Context4);
    
    ComPtr<ID3D11Texture2D> backBuffer;
    pImpl->swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    pImpl->d3d11Device->CreateRenderTargetView(backBuffer.Get(), nullptr, &pImpl->rtv);

    ComPtr<ID3DBlob> vsBlob, psBlob;
    D3DCompile(g_BlitShader, strlen(g_BlitShader), nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsBlob, nullptr);
    D3DCompile(g_BlitShader, strlen(g_BlitShader), nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psBlob, nullptr);
    pImpl->d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &pImpl->vs);
    pImpl->d3d11Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pImpl->ps);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    pImpl->d3d11Device->CreateSamplerState(&sampDesc, &pImpl->sampler);
    
    ComPtr<IDXGIDevice> dxgiDevice;
    pImpl->d3d11Device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    pImpl->adapterLuid = desc.AdapterLuid;
    
    ShowWindow(pImpl->hwnd, SW_SHOWDEFAULT);
    UpdateWindow(pImpl->hwnd);
}
Consumer::~Consumer() = default;

bool Consumer::process_events() {
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return IsWindow(pImpl->hwnd);
}

void Consumer::render_frame(const std::vector<std::shared_ptr<Producer>>& producers) {
    const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
    pImpl->d3d11Context->OMSetRenderTargets(1, pImpl->rtv.GetAddressOf(), nullptr);
    pImpl->d3d11Context->ClearRenderTargetView(pImpl->rtv.Get(), clearColor);

    if (producers.empty()) return;

    RECT clientRect;
    GetClientRect(pImpl->hwnd, &clientRect);
    float windowWidth = (float)(clientRect.right - clientRect.left);
    float windowHeight = (float)(clientRect.bottom - clientRect.top);
    float producerWidth = windowWidth / producers.size();

    pImpl->d3d11Context->VSSetShader(pImpl->vs.Get(), nullptr, 0);
    pImpl->d3d11Context->PSSetShader(pImpl->ps.Get(), nullptr, 0);
    pImpl->d3d11Context->PSSetSamplers(0, 1, pImpl->sampler.GetAddressOf());
    pImpl->d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    for (size_t i = 0; i < producers.size(); ++i) {
        if (producers[i]) {
            D3D11_VIEWPORT vp = { i * producerWidth, 0.0f, producerWidth, windowHeight, 0.0f, 1.0f };
            pImpl->d3d11Context->RSSetViewports(1, &vp);
            
            auto srv_ptr = reinterpret_cast<ID3D11ShaderResourceView*>(producers[i]->get_texture_ptr());
            if (srv_ptr) {
                pImpl->d3d11Context->PSSetShaderResources(0, 1, &srv_ptr);
                pImpl->d3d11Context->Draw(3, 0);
            }
        }
    }
}

void Consumer::present() { pImpl->swapChain->Present(1, 0); }

std::vector<ProducerInfo> Consumer::discover() {
    // Identical to previous correct version
    std::vector<ProducerInfo> discovered;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return discovered;

    PROCESSENTRY32W pe32 = { sizeof(PROCESSENTRY32W) };
    const std::vector<std::pair<std::wstring, std::wstring>> sigs = {
        { L"TegrityCam_Manifest_", L"D3D11" },
        { L"D3D12_Producer_Manifest_", L"D3D12" }
    };

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            for (const auto& sig : sigs) {
                std::wstring manifestName = sig.first + std::to_wstring(pe32.th32ProcessID);
                HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
                if (hManifest) {
                    BroadcastManifest* pView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                    if (pView) {
                        if (memcmp(&pView->adapterLuid, &pImpl->adapterLuid, sizeof(LUID)) == 0) {
                            discovered.push_back({ pe32.th32ProcessID, pe32.szExeFile, sig.second });
                        }
                        UnmapViewOfFile(pView);
                    }
                    CloseHandle(hManifest);
                }
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return discovered;
}

std::shared_ptr<Producer> Consumer::connect(unsigned long pid) {
    auto producerImpl = std::make_unique<ProducerImpl>();
    producerImpl->pid = pid;
    producerImpl->d3d11Device5 = pImpl->d3d11Device5;
    producerImpl->d3d11Context4 = pImpl->d3d11Context4;

    producerImpl->hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!producerImpl->hProcess) return nullptr;

    const std::vector<std::wstring> sigs = { L"TegrityCam_Manifest_", L"D3D12_Producer_Manifest_" };
    bool connected = false;

    for (const auto& sig : sigs) {
        std::wstring manifestName = sig + std::to_wstring(pid);
        producerImpl->hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
        if (producerImpl->hManifest) {
            producerImpl->pManifestView = (BroadcastManifest*)MapViewOfFile(producerImpl->hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
            if (producerImpl->pManifestView) {
                producerImpl->width = producerImpl->pManifestView->width;
                producerImpl->height = producerImpl->pManifestView->height;

                HANDLE hFence = GetHandleFromName_D3D12(producerImpl->pManifestView->fenceName);
                if (!hFence) continue;
                HRESULT hr = producerImpl->d3d11Device5->OpenSharedFence(hFence, IID_PPV_ARGS(&producerImpl->sharedFence));
                CloseHandle(hFence);
                if (FAILED(hr)) continue;

                hr = pImpl->d3d11Device1->OpenSharedResourceByName(producerImpl->pManifestView->textureName, DXGI_SHARED_RESOURCE_READ, IID_PPV_ARGS(&producerImpl->sharedTexture));
                if (FAILED(hr)) continue;
                
                hr = producerImpl->d3d11Device5->CreateShaderResourceView(producerImpl->sharedTexture.Get(), nullptr, &producerImpl->sharedSRV);
                if (FAILED(hr)) continue;

                connected = true;
                break;
            }
        }
    }

    if (!connected) return nullptr;
    return std::make_shared<Producer>(std::move(producerImpl));
}

Producer::Producer(std::unique_ptr<ProducerImpl> impl) : pImpl(std::move(impl)) {}
Producer::~Producer() = default;

bool Producer::wait_for_frame() {
    if (WaitForSingleObject(pImpl->hProcess, 0) != WAIT_TIMEOUT) return false;
    UINT64 latestFrame = pImpl->pManifestView->frameValue;
    if (latestFrame > pImpl->lastSeenFrame) {
        pImpl->d3d11Context4->Wait(pImpl->sharedFence.Get(), latestFrame);
        pImpl->lastSeenFrame = latestFrame;
    }
    return true;
}

uintptr_t Producer::get_texture_ptr() { return reinterpret_cast<uintptr_t>(pImpl->sharedSRV.Get()); }
unsigned int Producer::get_width() const { return pImpl->width; }
unsigned int Producer::get_height() const { return pImpl->height; }
unsigned long Producer::get_pid() const { return pImpl->pid; }