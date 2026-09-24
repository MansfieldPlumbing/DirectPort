#pragma once

#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <stdexcept>
#include <sstream>
#include <iomanip>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#define CHK(hr) if (FAILED(hr)) { std::stringstream ss; ss << "HRESULT failed: 0x" << std::hex << hr; throw std::runtime_error(ss.str()); }

// Forward-declare the Window class
class Window;

struct FrameData {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
    int channels = 0;
};

class DirectPortCamera {
public:
    DirectPortCamera();
    ~DirectPortCamera();

    // --- Original Methods (for NumPy/OpenCV) ---
    void init();
    void startCapture();
    void stopCapture();
    bool isRunning() const;
    FrameData getFrame();

    // --- NEW Windowing Methods ---
    std::shared_ptr<Window> create_window(int width, int height, const std::string& title);
    bool process_events(std::shared_ptr<Window> window);
    void render_frame_to_window(std::shared_ptr<Window> window);
    void present(std::shared_ptr<Window> window);

private:
    void createSourceReader();
    void init_d3d11(); // To initialize rendering device

    Microsoft::WRL::ComPtr<IMFSourceReader> m_sourceReader;
    std::atomic<bool> m_isCapturing = false;

    // --- NEW D3D11 Member Variables for Rendering ---
    Microsoft::WRL::ComPtr<ID3D11Device> m_d3d_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_d3d_context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_gpu_texture; // Texture to hold camera frame on GPU
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_gpu_texture_srv;

    // Blitting resources
    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_blitVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_blitPS;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_blitSampler;
};

// A simple handle to a window, managed by DirectPortCamera
class Window {
public:
    HWND hwnd;
    Microsoft::WRL::ComPtr<IDXGISwapChain> swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    int width;
    int height;
};