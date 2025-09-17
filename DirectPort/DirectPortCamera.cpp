#include "DirectPortCamera.h"
#include <d3dcompiler.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using namespace Microsoft::WRL;

// Forward declaration for the window procedure
LRESULT CALLBACK DPCameraWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

const char* g_dpcBlitShaderHLSL = R"(
    Texture2D    g_texture : register(t0); SamplerState g_sampler : register(s0);
    struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
    PSInput VSMain(uint id : SV_VertexID) {
        PSInput r; float2 uv = float2((id << 1) & 2, id & 2);
        r.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0, 1); r.uv = uv; return r;
    }
    float4 PSMain(PSInput i) : SV_TARGET { return g_texture.Sample(g_sampler, i.uv); }
)";


DirectPortCamera::DirectPortCamera() {
    CHK(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED));
    CHK(MFStartup(MF_VERSION, MFSTARTUP_FULL));
}

DirectPortCamera::~DirectPortCamera() {
    stopCapture();
    MFShutdown();
    CoUninitialize();
}

void DirectPortCamera::init() {
    createSourceReader();
}

// --- Original Methods Implementation ---

void DirectPortCamera::createSourceReader() {
    ComPtr<IMFAttributes> attributes;
    CHK(MFCreateAttributes(&attributes, 1));
    CHK(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    CHK(MFEnumDeviceSources(attributes.Get(), &devices, &count));
    if (count == 0) {
        throw std::runtime_error("No camera devices found.");
    }

    ComPtr<IMFMediaSource> mediaSource;
    CHK(devices[0]->ActivateObject(IID_PPV_ARGS(&mediaSource)));
    for (UINT32 i = 0; i < count; i++) {
        devices[i]->Release();
    }
    CoTaskMemFree(devices);

    // Use config attributes from your original code
    ComPtr<IMFAttributes> configAttributes;
    CHK(MFCreateAttributes(&configAttributes, 2));
    CHK(configAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE));
    CHK(configAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE));
    CHK(MFCreateSourceReaderFromMediaSource(mediaSource.Get(), configAttributes.Get(), &m_sourceReader));

    // --- THIS IS THE VERBATIM CORRECT LOGIC FROM YOUR WORKING FILE ---
    ComPtr<IMFMediaType> outputType;
    CHK(MFCreateMediaType(&outputType));
    CHK(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHK(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));

    bool found_compatible_format = false;
    for (DWORD i = 0; ; ++i) {
        ComPtr<IMFMediaType> nativeType;
        HRESULT hr = m_sourceReader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType);
        if (hr == MF_E_NO_MORE_TYPES) {
            break;
        }
        CHK(hr);

        hr = m_sourceReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, nativeType.Get());
        if (SUCCEEDED(hr)) {
            hr = m_sourceReader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, outputType.Get());
            if (SUCCEEDED(hr)) {
                found_compatible_format = true;
                break;
            }
        }
    }

    if (!found_compatible_format) {
        throw std::runtime_error("Could not find a compatible media type for the camera that can be converted to RGB32.");
    }
}


void DirectPortCamera::startCapture() { m_isCapturing = true; }
void DirectPortCamera::stopCapture() { m_isCapturing = false; }
bool DirectPortCamera::isRunning() const { return m_isCapturing; }

FrameData DirectPortCamera::getFrame() {
    if (!m_isCapturing || !m_sourceReader) return {};
    ComPtr<IMFSample> pSample;
    DWORD streamFlags;
    LONGLONG timestamp;
    HRESULT hr = m_sourceReader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, NULL, &streamFlags, &timestamp, &pSample);
    if (FAILED(hr) || !pSample) return {};

    ComPtr<IMFMediaBuffer> pBuffer;
    CHK(pSample->ConvertToContiguousBuffer(&pBuffer));

    BYTE* pData = nullptr;
    DWORD currentLength = 0;
    CHK(pBuffer->Lock(&pData, NULL, &currentLength));

    FrameData frame;
    frame.data.assign(pData, pData + currentLength);

    CHK(pBuffer->Unlock());

    ComPtr<IMFMediaType> pType;
    CHK(m_sourceReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pType));
    UINT32 width, height;
    CHK(MFGetAttributeSize(pType.Get(), MF_MT_FRAME_SIZE, &width, &height));
    frame.width = width; frame.height = height; frame.channels = 4;
    return frame;
}


// --- NEW Windowing Methods Implementation ---

void DirectPortCamera::init_d3d11() {
    if (m_d3d_device) return; // Already initialized

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    CHK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &m_d3d_device, nullptr, &m_d3d_context));

    ComPtr<ID3DBlob> vsBlob, psBlob;
    CHK(D3DCompile(g_dpcBlitShaderHLSL, strlen(g_dpcBlitShaderHLSL), 0, 0, 0, "VSMain", "vs_5_0", 0, 0, &vsBlob, 0));
    CHK(D3DCompile(g_dpcBlitShaderHLSL, strlen(g_dpcBlitShaderHLSL), 0, 0, 0, "PSMain", "ps_5_0", 0, 0, &psBlob, 0));
    CHK(m_d3d_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &m_blitVS));
    CHK(m_d3d_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &m_blitPS));

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP; sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    CHK(m_d3d_device->CreateSamplerState(&sampDesc, &m_blitSampler));
}

std::shared_ptr<Window> DirectPortCamera::create_window(int width, int height, const std::string& title) {
    init_d3d11(); // Ensure D3D device is ready

    auto win = std::make_shared<Window>();
    win->width = width;
    win->height = height;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, DPCameraWindowProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"DirectPortCameraWindow", NULL };
    RegisterClassExW(&wc);
    std::wstring wtitle(title.begin(), title.end());
    win->hwnd = CreateWindowW(wc.lpszClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW, 100, 100, width, height, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2; sd.BufferDesc.Width = width; sd.BufferDesc.Height = height; sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = win->hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    
    ComPtr<IDXGIFactory> factory;
    CHK(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory));
    CHK(factory->CreateSwapChain(m_d3d_device.Get(), &sd, &win->swapChain));

    ComPtr<ID3D11Texture2D> pBackBuffer;
    CHK(win->swapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer)));
    CHK(m_d3d_device->CreateRenderTargetView(pBackBuffer.Get(), NULL, &win->rtv));

    ShowWindow(win->hwnd, SW_SHOWDEFAULT);
    UpdateWindow(win->hwnd);
    return win;
}

bool DirectPortCamera::process_events(std::shared_ptr<Window> window) {
    if (!window || !window->hwnd) return false;
    MSG msg = {};
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { window->hwnd = nullptr; return false; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return IsWindow(window->hwnd);
}

void DirectPortCamera::render_frame_to_window(std::shared_ptr<Window> window) {
    if (!window || !isRunning()) return;

    FrameData frame = getFrame();
    if (frame.data.empty()) return;

    if (!m_gpu_texture) {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = frame.width; texDesc.Height = frame.height; texDesc.MipLevels = 1; texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; texDesc.SampleDesc.Count = 1; texDesc.Usage = D3D11_USAGE_DEFAULT; texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CHK(m_d3d_device->CreateTexture2D(&texDesc, nullptr, &m_gpu_texture));
        CHK(m_d3d_device->CreateShaderResourceView(m_gpu_texture.Get(), nullptr, &m_gpu_texture_srv));
    }

    m_d3d_context->UpdateSubresource(m_gpu_texture.Get(), 0, NULL, frame.data.data(), frame.width * 4, 0);

    m_d3d_context->OMSetRenderTargets(1, window->rtv.GetAddressOf(), NULL);
    D3D11_VIEWPORT vp; vp.Width = (FLOAT)window->width; vp.Height = (FLOAT)window->height; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f; vp.TopLeftX = 0; vp.TopLeftY = 0;
    m_d3d_context->RSSetViewports(1, &vp);
    m_d3d_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_d3d_context->VSSetShader(m_blitVS.Get(), NULL, 0);
    m_d3d_context->PSSetShader(m_blitPS.Get(), NULL, 0);
    m_d3d_context->PSSetShaderResources(0, 1, m_gpu_texture_srv.GetAddressOf());
    m_d3d_context->PSSetSamplers(0, 1, m_blitSampler.GetAddressOf());
    m_d3d_context->Draw(3, 0);
}

void DirectPortCamera::present(std::shared_ptr<Window> window) {
    if (window && window->swapChain) {
        window->swapChain->Present(1, 0); // VSync on
    }
}

LRESULT CALLBACK DPCameraWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}