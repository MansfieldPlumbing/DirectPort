#include "DirectPortGL.h"
#include <stdexcept>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <sddl.h>
#include <tlhelp32.h>

#include <dxgi1_6.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include "d3dx12.h"
#include <wrl/client.h>

#include <GL/glew.h>
#include <GL/wglew.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "Synchronization.lib")

#ifndef GL_HANDLE_TYPE_D3D11_IMAGE_EXT
#define GL_HANDLE_TYPE_D3D11_IMAGE_EXT 0x958A
#endif
#ifndef GL_HANDLE_TYPE_D3D11_FENCE_EXT
#define GL_HANDLE_TYPE_D3D11_FENCE_EXT 0x9594
#endif
#ifndef GL_LAYOUT_SHADER_READ_ONLY_EXT
#define GL_LAYOUT_SHADER_READ_ONLY_EXT 0x95B1
#endif

using namespace Microsoft::WRL;

namespace DirectPortGL {

namespace {
    std::wstring string_to_wstring(const std::string& str) {
        if (str.empty()) return std::wstring();
        int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
        std::wstring wstr(size, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), wstr.data(), size);
        return wstr;
    }
    
    std::string wstring_to_string(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
        std::string strTo(size_needed, 0);
        WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
        return strTo;
    }

    void validate_stream_name(const std::string& name) {
        if (name.empty() || name.length() > 64) {
            throw std::invalid_argument("Stream name must be 1-64 characters.");
        }
        for (char c : name) {
            if (!isalnum(c) && c != '_' && c != '-') {
                throw std::invalid_argument("Stream name can only contain alphanumerics, underscores, and hyphens.");
            }
        }
    }

    bool get_manifest_from_pid(DWORD pid, BroadcastManifest& manifest, std::wstring& found_name) {
        const std::vector<std::wstring> prefixes = { L"DirectPortGL_Producer_Manifest_", L"DirectPort_Producer_Manifest_", L"D3D12_Producer_Manifest_" };
        for (const auto& prefix : prefixes) {
            std::wstring manifestName = prefix + std::to_wstring(pid);
            HANDLE hManifest = OpenFileMappingW(FILE_MAP_READ, FALSE, manifestName.c_str());
            if (hManifest) {
                BroadcastManifest* pView = (BroadcastManifest*)MapViewOfFile(hManifest, FILE_MAP_READ, 0, 0, sizeof(BroadcastManifest));
                if (pView) {
                    memcpy(&manifest, pView, sizeof(BroadcastManifest));
                    UnmapViewOfFile(pView);
                    CloseHandle(hManifest);
                    found_name = manifestName;
                    return true;
                }
                CloseHandle(hManifest);
            }
        }
        return false;
    }

    void CheckGLError(const char* stmt, const char* fname, int line) {
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::string error_str = "OpenGL error " + std::to_string(err) + " at " + fname + ":" + std::to_string(line) + " - for " + stmt;
            throw std::runtime_error(error_str);
        }
    }
    #define GL_CHECK(stmt) do { stmt; CheckGLError(#stmt, __FILE__, __LINE__); } while (0)

    const char* g_blitShaderGLSL_VS = "#version 330 core\nout vec2 UV; void main(){float x=-1.0+float((gl_VertexID&1)<<2);float y=-1.0+float((gl_VertexID&2)<<1);gl_Position=vec4(x,y,0.0,1.0);UV=(gl_Position.xy+1.0)/2.0;}";
    const char* g_blitShaderGLSL_FS = "#version 330 core\nin vec2 UV; out vec4 color; uniform sampler2D tex; void main(){color=texture(tex,UV);}";
    
    PFNGLCREATEMEMORYOBJECTSEXTPROC glCreateMemoryObjectsEXT = nullptr;
    PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC glImportMemoryWin32HandleEXT = nullptr;
    PFNGLTEXTURESTORAGEMEM2DEXTPROC glTextureStorageMem2DEXT = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC glDeleteMemoryObjectsEXT = nullptr;
    PFNGLGENSEMAPHORESEXTPROC glGenSemaphoresEXT = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC glDeleteSemaphoresEXT = nullptr;
    PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC glImportSemaphoreWin32HandleEXT = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC glWaitSemaphoreEXT = nullptr;
    PFNGLSIGNALSEMAPHOREEXTPROC glSignalSemaphoreEXT = nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DPROC glFramebufferTexture2D = nullptr;

    void load_gl_extensions() {
        glCreateMemoryObjectsEXT = (PFNGLCREATEMEMORYOBJECTSEXTPROC)wglGetProcAddress("glCreateMemoryObjectsEXT");
        glImportMemoryWin32HandleEXT = (PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC)wglGetProcAddress("glImportMemoryWin32HandleEXT");
        glTextureStorageMem2DEXT = (PFNGLTEXTURESTORAGEMEM2DEXTPROC)wglGetProcAddress("glTextureStorageMem2DEXT");
        glDeleteMemoryObjectsEXT = (PFNGLDELETEMEMORYOBJECTSEXTPROC)wglGetProcAddress("glDeleteMemoryObjectsEXT");
        glGenSemaphoresEXT = (PFNGLGENSEMAPHORESEXTPROC)wglGetProcAddress("glGenSemaphoresEXT");
        glDeleteSemaphoresEXT = (PFNGLDELETESEMAPHORESEXTPROC)wglGetProcAddress("glDeleteSemaphoresEXT");
        glImportSemaphoreWin32HandleEXT = (PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC)wglGetProcAddress("glImportSemaphoreWin32HandleEXT");
        glWaitSemaphoreEXT = (PFNGLWAITSEMAPHOREEXTPROC)wglGetProcAddress("glWaitSemaphoreEXT");
        glSignalSemaphoreEXT = (PFNGLSIGNALSEMAPHOREEXTPROC)wglGetProcAddress("glSignalSemaphoreEXT");
        glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)wglGetProcAddress("glFramebufferTexture2D");

        if (!glCreateMemoryObjectsEXT || !glImportMemoryWin32HandleEXT || !glTextureStorageMem2DEXT)
            throw std::runtime_error("GL_EXT_memory_object_win32 not supported or failed to load.");
        if (!glGenSemaphoresEXT || !glImportSemaphoreWin32HandleEXT || !glWaitSemaphoreEXT || !glSignalSemaphoreEXT)
            throw std::runtime_error("GL_EXT_semaphore_win32 not supported or failed to load.");
        if (!glFramebufferTexture2D)
            throw std::runtime_error("Framebuffer extensions not supported or failed to load.");
    }
}

struct TextureGL::Impl {
    GLuint textureID = 0;
    GLuint memoryObject = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

struct WindowGL::Impl {
    HWND hwnd = nullptr;
    HDC hdc = nullptr;
    HGLRC hglrc = nullptr;
    DeviceGL::Impl* device_pImpl = nullptr;
};

struct DeviceGL::Impl {
    HGLRC master_glrc = nullptr;
    int master_pixel_format_id = 0;
    GLuint blitProgram = 0;
    GLuint blitVAO = 0;
    GLuint fbo = 0;
    std::map<std::string, GLuint> shaderCache;
    
    ComPtr<ID3D11Device> d3d_device;
    ComPtr<ID3D11Device1> d3d_device1;
    ComPtr<ID3D11Device5> d3d_device5;
    ComPtr<ID3D11DeviceContext4> d3d_context4;
    LUID adapterLuid;

    ~Impl() {
        if (!master_glrc) return;
        
        HGLRC current_ctx = wglGetCurrentContext();
        HDC current_dc = wglGetCurrentDC();
        if (master_glrc) wglMakeCurrent(current_dc, master_glrc);

        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (blitProgram) glDeleteProgram(blitProgram);
        if (blitVAO) glDeleteVertexArrays(1, &blitVAO);
        for (auto const& [key, val] : shaderCache) {
            glDeleteProgram(val);
        }
        
        wglMakeCurrent(current_dc, current_ctx);
    }
};

struct ProducerGL::Impl {
    std::string stream_name;
    HANDLE hManifest = nullptr;
    BroadcastManifest* pManifestView = nullptr;
    UINT64 frameValue = 0;
    DWORD pid = 0;
    
    std::shared_ptr<TextureGL> sourceTexture;
    ComPtr<ID3D11Texture2D> d3dSharedTexture;
    
    ComPtr<ID3D11Fence> d3d11Fence;
    ID3D11DeviceContext4* pDeviceContext = nullptr;
    
    HANDLE hDxDevice = nullptr;
    HANDLE hDxObject = nullptr;
};

struct ConsumerGL::Impl {
    DWORD pid = 0;
    HANDLE hProcess = nullptr;
    UINT64 lastSeenFrame = 0;
    BroadcastManifest currentManifest;
    std::shared_ptr<TextureGL> privateTexture;
    GLuint glSemaphore = 0;
};

LRESULT CALLBACK GLWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (uMsg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

DeviceGL::DeviceGL() : pImpl(std::make_unique<Impl>()) {}
DeviceGL::~DeviceGL() = default;

std::shared_ptr<DeviceGL> DeviceGL::create() {
    auto self = std::shared_ptr<DeviceGL>(new DeviceGL());
    
    WNDCLASSEXW wc_temp = { sizeof(WNDCLASSEXW), CS_OWNDC, DefWindowProcW, 0, 0, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"DirectPortGLTempWindowClass", NULL };
    RegisterClassExW(&wc_temp);
    HWND hwnd_temp = CreateWindowW(wc_temp.lpszClassName, L"Temp", 0, 0, 0, 1, 1, NULL, NULL, wc_temp.hInstance, NULL);
    if (!hwnd_temp) throw std::runtime_error("Failed to create temporary window for pixel format selection.");

    HDC hdc_temp = GetDC(hwnd_temp);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(PIXELFORMATDESCRIPTOR), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    int pf_id = ChoosePixelFormat(hdc_temp, &pfd);
    if (pf_id == 0) {
        DestroyWindow(hwnd_temp);
        throw std::runtime_error("ChoosePixelFormat failed.");
    }
    self->pImpl->master_pixel_format_id = pf_id;
    ReleaseDC(hwnd_temp, hdc_temp);
    DestroyWindow(hwnd_temp);
    
    UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    d3dFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    ID3D11DeviceContext* tempContext = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, d3dFlags, nullptr, 0, D3D11_SDK_VERSION, &self->pImpl->d3d_device, nullptr, &tempContext);
    if (FAILED(hr)) throw std::runtime_error("Failed to create D3D11 device for interop.");
    
    self->pImpl->d3d_device.As(&self->pImpl->d3d_device1);
    self->pImpl->d3d_device.As(&self->pImpl->d3d_device5);
    tempContext->QueryInterface(IID_PPV_ARGS(&self->pImpl->d3d_context4));
    tempContext->Release();
    if (!self->pImpl->d3d_device5 || !self->pImpl->d3d_context4) throw std::runtime_error("D3D11.5 interfaces (required for fences) not supported.");
    
    ComPtr<IDXGIDevice> dxgiDevice;
    self->pImpl->d3d_device.As(&dxgiDevice);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc;
    adapter->GetDesc(&desc);
    self->pImpl->adapterLuid = desc.AdapterLuid;
    
    return self;
}

std::shared_ptr<WindowGL> DeviceGL::create_window(uint32_t width, uint32_t height, const std::string& title) {
    auto win = std::shared_ptr<WindowGL>(new WindowGL());
    win->pImpl->device_pImpl = pImpl.get();

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_OWNDC, GLWindowProc, 0, 0, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"DirectPortGLWindowClass", NULL };
    RegisterClassExW(&wc);
    std::wstring wtitle = string_to_wstring(title);
    win->pImpl->hwnd = CreateWindowW(wc.lpszClassName, wtitle.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, NULL, NULL, wc.hInstance, NULL);
    if (!win->pImpl->hwnd) throw std::runtime_error("Failed to create window.");
    win->pImpl->hdc = GetDC(win->pImpl->hwnd);
    
    PIXELFORMATDESCRIPTOR pfd = { sizeof(PIXELFORMATDESCRIPTOR), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 24, 8, 0, PFD_MAIN_PLANE, 0, 0, 0, 0 };
    if (!SetPixelFormat(win->pImpl->hdc, pImpl->master_pixel_format_id, &pfd)) {
        throw std::runtime_error("SetPixelFormat failed.");
    }

    win->pImpl->hglrc = wglCreateContext(win->pImpl->hdc);
    if (!win->pImpl->hglrc) throw std::runtime_error("Failed to create OpenGL context.");

    if (pImpl->master_glrc == nullptr) {
        pImpl->master_glrc = win->pImpl->hglrc;
        win->make_current();

        if (glewInit() != GLEW_OK) throw std::runtime_error("Failed to initialize GLEW.");
        load_gl_extensions();
    
        if (!wglDXOpenDeviceNV || !wglDXRegisterObjectNV || !wglDXLockObjectsNV || !wglDXUnlockObjectsNV || !wglDXCloseDeviceNV) {
            throw std::runtime_error("WGL_NV_DX_interop not supported.");
        }
        
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &g_blitShaderGLSL_VS, NULL); glCompileShader(vs);
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &g_blitShaderGLSL_FS, NULL); glCompileShader(fs);
        pImpl->blitProgram = glCreateProgram();
        glAttachShader(pImpl->blitProgram, vs); glAttachShader(pImpl->blitProgram, fs);
        glLinkProgram(pImpl->blitProgram);
        glDeleteShader(vs); glDeleteShader(fs);
        glGenVertexArrays(1, &pImpl->blitVAO);
        glGenFramebuffers(1, &pImpl->fbo);

    } else {
        if (!wglShareLists(pImpl->master_glrc, win->pImpl->hglrc)) {
            wglDeleteContext(win->pImpl->hglrc);
            ReleaseDC(win->pImpl->hwnd, win->pImpl->hdc);
            DestroyWindow(win->pImpl->hwnd);
            throw std::runtime_error("Failed to share OpenGL contexts (wglShareLists).");
        }
    }

    ShowWindow(win->pImpl->hwnd, SW_SHOW);
    UpdateWindow(win->pImpl->hwnd);
    return win;
}

std::shared_ptr<TextureGL> DeviceGL::create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data) {
    auto tex = std::shared_ptr<TextureGL>(new TextureGL());
    tex->pImpl->width = width;
    tex->pImpl->height = height;
    tex->pImpl->format = format;

    GLenum gl_internal_format, gl_format, gl_type;
    switch(format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: gl_internal_format = GL_RGBA8; gl_format = GL_BGRA; gl_type = GL_UNSIGNED_BYTE; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: gl_internal_format = GL_RGBA8; gl_format = GL_RGBA; gl_type = GL_UNSIGNED_BYTE; break;
        case DXGI_FORMAT_R32_FLOAT: gl_internal_format = GL_R32F; gl_format = GL_RED; gl_type = GL_FLOAT; break;
        default: throw std::runtime_error("Unsupported texture format for OpenGL.");
    }
    
    GL_CHECK(glGenTextures(1, &tex->pImpl->textureID));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, tex->pImpl->textureID));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL_CHECK(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL_CHECK(glTexImage2D(GL_TEXTURE_2D, 0, gl_internal_format, width, height, 0, gl_format, gl_type, data));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, 0));
    
    return tex;
}

std::shared_ptr<ProducerGL> DeviceGL::create_producer(const std::string& stream_name, std::shared_ptr<TextureGL> texture) {
    validate_stream_name(stream_name);
    auto producer = std::shared_ptr<ProducerGL>(new ProducerGL());
    producer->pImpl->stream_name = stream_name;
    producer->pImpl->pid = GetCurrentProcessId();
    producer->pImpl->sourceTexture = texture;
    producer->pImpl->pDeviceContext = pImpl->d3d_context4.Get();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = texture->get_width();
    desc.Height = texture->get_height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = texture->pImpl->format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    if (FAILED(pImpl->d3d_device->CreateTexture2D(&desc, nullptr, &producer->pImpl->d3dSharedTexture))) {
        throw std::runtime_error("Failed to create shared D3D11 texture.");
    }

    ComPtr<IDXGIResource1> dxgiResource;
    producer->pImpl->d3dSharedTexture.As(&dxgiResource);
    HANDLE sharedTextureHandle;
    std::wstring textureName = L"Global\\DirectPort_Texture_" + std::to_wstring(producer->pImpl->pid) + L"_" + string_to_wstring(stream_name);
    if (FAILED(dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, textureName.c_str(), &sharedTextureHandle))) {
        throw std::runtime_error("Failed to create named shared handle for texture.");
    }

    if (FAILED(pImpl->d3d_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&producer->pImpl->d3d11Fence)))) {
        throw std::runtime_error("Failed to create shared D3D11 fence.");
    }
    HANDLE sharedFenceHandle;
    std::wstring fenceName = L"Global\\DirectPort_Fence_" + std::to_wstring(producer->pImpl->pid) + L"_" + string_to_wstring(stream_name);
    if (FAILED(producer->pImpl->d3d11Fence->CreateSharedHandle(nullptr, GENERIC_ALL, fenceName.c_str(), &sharedFenceHandle))) {
        throw std::runtime_error("Failed to create named shared handle for fence.");
    }

    std::wstring manifestName = L"DirectPort_Producer_Manifest_" + std::to_wstring(producer->pImpl->pid);
    producer->pImpl->hManifest = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(BroadcastManifest), manifestName.c_str());
    if (!producer->pImpl->hManifest) throw std::runtime_error("Failed to create manifest file mapping.");
    
    producer->pImpl->pManifestView = (BroadcastManifest*)MapViewOfFile(producer->pImpl->hManifest, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BroadcastManifest));
    if (!producer->pImpl->pManifestView) throw std::runtime_error("Failed to map view of manifest file.");
    
    wcscpy_s(producer->pImpl->pManifestView->textureName, textureName.c_str());
    wcscpy_s(producer->pImpl->pManifestView->fenceName, fenceName.c_str());
    producer->pImpl->pManifestView->width = texture->get_width();
    producer->pImpl->pManifestView->height = texture->get_height();
    producer->pImpl->pManifestView->format = texture->pImpl->format;
    producer->pImpl->pManifestView->adapterLuid = pImpl->adapterLuid;
    producer->pImpl->pManifestView->frameValue = 0;
    
    producer->pImpl->hDxDevice = wglDXOpenDeviceNV(pImpl->d3d_device.Get());
    if(!producer->pImpl->hDxDevice) throw std::runtime_error("wglDXOpenDeviceNV failed.");
    
    producer->pImpl->hDxObject = wglDXRegisterObjectNV(producer->pImpl->hDxDevice, producer->pImpl->d3dSharedTexture.Get(), texture->get_gl_texture_id(), GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV);
    if(!producer->pImpl->hDxObject) throw std::runtime_error("wglDXRegisterObjectNV failed.");

    CloseHandle(sharedTextureHandle);
    CloseHandle(sharedFenceHandle);

    return producer;
}

std::shared_ptr<ConsumerGL> DeviceGL::connect_to_producer(unsigned long pid) {
    auto consumer = std::shared_ptr<ConsumerGL>(new ConsumerGL());
    consumer->pImpl->pid = pid;
    std::wstring manifest_name;

    if (!get_manifest_from_pid(pid, consumer->pImpl->currentManifest, manifest_name)) {
        return nullptr;
    }

    if (memcmp(&consumer->pImpl->currentManifest.adapterLuid, &pImpl->adapterLuid, sizeof(LUID)) != 0) {
        throw std::runtime_error("Producer and consumer are on different GPUs.");
    }
    
    consumer->pImpl->hProcess = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!consumer->pImpl->hProcess && GetLastError() != ERROR_ACCESS_DENIED) {
    }

    ComPtr<ID3D12Device> tempD3D12Device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&tempD3D12Device)))) {
        throw std::runtime_error("Failed to create temporary D3D12 device for handle lookup.");
    }

    HANDLE sharedHandle;
    if (FAILED(tempD3D12Device->OpenSharedHandleByName(consumer->pImpl->currentManifest.textureName, GENERIC_ALL, &sharedHandle))) {
        throw std::runtime_error("Failed to open shared texture handle by name via D3D12. Ensure producer is running.");
    }

    GLenum gl_internal_format;
    switch(consumer->pImpl->currentManifest.format) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: gl_internal_format = GL_RGBA8; break;
        case DXGI_FORMAT_R8G8B8A8_UNORM: gl_internal_format = GL_RGBA8; break;
        case DXGI_FORMAT_R32_FLOAT: gl_internal_format = GL_R32F; break;
        default: throw std::runtime_error("Unsupported shared texture format.");
    }

    consumer->pImpl->privateTexture = std::shared_ptr<TextureGL>(new TextureGL());
    consumer->pImpl->privateTexture->pImpl->width = consumer->pImpl->currentManifest.width;
    consumer->pImpl->privateTexture->pImpl->height = consumer->pImpl->currentManifest.height;
    consumer->pImpl->privateTexture->pImpl->format = consumer->pImpl->currentManifest.format;

    GL_CHECK(glGenTextures(1, &consumer->pImpl->privateTexture->pImpl->textureID));
    GL_CHECK(glCreateMemoryObjectsEXT(1, &consumer->pImpl->privateTexture->pImpl->memoryObject));
    GL_CHECK(glImportMemoryWin32HandleEXT(consumer->pImpl->privateTexture->pImpl->memoryObject, 0, GL_HANDLE_TYPE_D3D11_IMAGE_EXT, sharedHandle));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, consumer->pImpl->privateTexture->pImpl->textureID));
    GL_CHECK(glTextureStorageMem2DEXT(consumer->pImpl->privateTexture->pImpl->textureID, 1, gl_internal_format, consumer->pImpl->privateTexture->pImpl->width, consumer->pImpl->privateTexture->pImpl->height, consumer->pImpl->privateTexture->pImpl->memoryObject, 0));
    CloseHandle(sharedHandle);
    
    HANDLE fenceHandle;
    if(!tempD3D12Device || FAILED(tempD3D12Device->OpenSharedHandleByName(consumer->pImpl->currentManifest.fenceName, GENERIC_ALL, &fenceHandle))) {
        throw std::runtime_error("Failed to open shared fence handle by name via D3D12.");
    }

    GL_CHECK(glGenSemaphoresEXT(1, &consumer->pImpl->glSemaphore));
    GL_CHECK(glImportSemaphoreWin32HandleEXT(consumer->pImpl->glSemaphore, GL_HANDLE_TYPE_D3D11_FENCE_EXT, fenceHandle));
    CloseHandle(fenceHandle);

    return consumer;
}

void DeviceGL::blit(std::shared_ptr<TextureGL> source, std::shared_ptr<WindowGL> destination) {
    destination->make_current();
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL_CHECK(glViewport(0, 0, destination->get_width(), destination->get_height()));
    GL_CHECK(glUseProgram(pImpl->blitProgram));
    GL_CHECK(glActiveTexture(GL_TEXTURE0));
    GL_CHECK(glBindTexture(GL_TEXTURE_2D, source->get_gl_texture_id()));
    GL_CHECK(glBindVertexArray(pImpl->blitVAO));
    GL_CHECK(glDrawArrays(GL_TRIANGLES, 0, 3));
    GL_CHECK(glBindVertexArray(0));
}

void DeviceGL::clear(std::shared_ptr<WindowGL> window, float r, float g, float b, float a) {
    window->make_current();
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GL_CHECK(glClearColor(r, g, b, a));
    GL_CHECK(glClear(GL_COLOR_BUFFER_BIT));
}

void DeviceGL::copy_texture(std::shared_ptr<TextureGL> source, std::shared_ptr<TextureGL> destination) {
    if (!source || !destination) return;
    GL_CHECK(glCopyImageSubData(source->get_gl_texture_id(), GL_TEXTURE_2D, 0, 0, 0, 0,
        destination->get_gl_texture_id(), GL_TEXTURE_2D, 0, 0, 0, 0,
        source->get_width(), source->get_height(), 1));
}

void DeviceGL::blit_texture_to_region(std::shared_ptr<TextureGL> source, std::shared_ptr<TextureGL> destination,
    uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height) {
    GL_CHECK(glBindFramebuffer(GL_READ_FRAMEBUFFER, pImpl->fbo));
    GL_CHECK(glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source->get_gl_texture_id(), 0));
    GL_CHECK(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, pImpl->fbo));
    GL_CHECK(glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destination->get_gl_texture_id(), 0));

    GL_CHECK(glBlitFramebuffer(0, 0, source->get_width(), source->get_height(),
        dest_x, dest_y, dest_x + dest_width, dest_y + dest_height,
        GL_COLOR_BUFFER_BIT, GL_LINEAR));

    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

void DeviceGL::apply_shader(std::shared_ptr<TextureGL> output, const std::string& glsl_fragment_shader, const std::vector<std::shared_ptr<TextureGL>>& inputs, const std::vector<uint8_t>& constants) {
    GLuint program = 0;
    if (pImpl->shaderCache.count(glsl_fragment_shader)) {
        program = pImpl->shaderCache[glsl_fragment_shader];
    } else {
        GLuint vs = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vs, 1, &g_blitShaderGLSL_VS, NULL); glCompileShader(vs);

        const char* fs_str = glsl_fragment_shader.c_str();
        GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fs, 1, &fs_str, NULL); glCompileShader(fs);

        GLint status;
        glGetShaderiv(fs, GL_COMPILE_STATUS, &status);
        if (status == GL_FALSE) {
            GLint len; glGetShaderiv(fs, GL_INFO_LOG_LENGTH, &len);
            std::vector<char> log(len); glGetShaderInfoLog(fs, len, NULL, log.data());
            glDeleteShader(vs); glDeleteShader(fs);
            throw std::runtime_error("Shader compilation failed: " + std::string(log.data()));
        }
        program = glCreateProgram();
        glAttachShader(program, vs); glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs); glDeleteShader(fs);
        pImpl->shaderCache[glsl_fragment_shader] = program;
    }

    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, pImpl->fbo));
    GL_CHECK(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, output->get_gl_texture_id(), 0));
    GL_CHECK(glViewport(0, 0, output->get_width(), output->get_height()));
    GL_CHECK(glUseProgram(program));

    for (size_t i = 0; i < inputs.size(); ++i) {
        std::string name = "tex" + std::to_string(i);
        GLint loc = glGetUniformLocation(program, name.c_str());
        if (loc != -1) {
            glUniform1i(loc, (GLint)i);
            glActiveTexture(GL_TEXTURE0 + (GLenum)i);
            glBindTexture(GL_TEXTURE_2D, inputs[i]->get_gl_texture_id());
        }
    }
    if (!constants.empty()) {
        GLint loc = glGetUniformLocation(program, "constants");
        if (loc != -1) {
            glUniform1fv(loc, (GLsizei)(constants.size() / sizeof(float)), (const GLfloat*)constants.data());
        }
    }

    GL_CHECK(glBindVertexArray(pImpl->blitVAO));
    GL_CHECK(glDrawArrays(GL_TRIANGLES, 0, 3));
    GL_CHECK(glBindVertexArray(0));
    GL_CHECK(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

TextureGL::TextureGL() : pImpl(std::make_unique<Impl>()) {}
TextureGL::~TextureGL() {
    if (pImpl->textureID) glDeleteTextures(1, &pImpl->textureID);
    if (pImpl->memoryObject) glDeleteMemoryObjectsEXT(1, &pImpl->memoryObject);
}
uint32_t TextureGL::get_width() const { return pImpl->width; }
uint32_t TextureGL::get_height() const { return pImpl->height; }
uint32_t TextureGL::get_gl_texture_id() const { return pImpl->textureID; }

WindowGL::WindowGL() : pImpl(std::make_unique<Impl>()) {}
WindowGL::~WindowGL() {
    if (pImpl->hglrc) {
        if (pImpl->device_pImpl && pImpl->device_pImpl->master_glrc == pImpl->hglrc) {
            pImpl->device_pImpl->master_glrc = nullptr;
        }
        wglDeleteContext(pImpl->hglrc);
    }
    if (pImpl->hdc) ReleaseDC(pImpl->hwnd, pImpl->hdc);
    if (pImpl->hwnd) DestroyWindow(pImpl->hwnd);
}
void WindowGL::make_current() {
    if (wglGetCurrentContext() != pImpl->hglrc) {
        wglMakeCurrent(pImpl->hdc, pImpl->hglrc);
    }
}
bool WindowGL::process_events() {
    MSG msg = {};
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return IsWindow(pImpl->hwnd);
}
void WindowGL::present() { SwapBuffers(pImpl->hdc); }
void WindowGL::set_title(const std::string& title) { SetWindowTextW(pImpl->hwnd, string_to_wstring(title).c_str()); }
uint32_t WindowGL::get_width() const { RECT r; if(GetClientRect(pImpl->hwnd, &r)) return r.right - r.left; return 0; }
uint32_t WindowGL::get_height() const { RECT r; if(GetClientRect(pImpl->hwnd, &r)) return r.bottom - r.top; return 0; }

ProducerGL::ProducerGL() : pImpl(std::make_unique<Impl>()) {}
ProducerGL::~ProducerGL() {
    if(pImpl->hDxObject) wglDXUnregisterObjectNV(pImpl->hDxDevice, pImpl->hDxObject);
    if(pImpl->hDxDevice) wglDXCloseDeviceNV(pImpl->hDxDevice);
    if (pImpl->pManifestView) UnmapViewOfFile(pImpl->pManifestView);
    if (pImpl->hManifest) CloseHandle(pImpl->hManifest);
}

void ProducerGL::signal_frame() {
    if (!pImpl->sourceTexture) return;

    if(!wglDXLockObjectsNV(pImpl->hDxDevice, 1, &pImpl->hDxObject)) return;
    GL_CHECK(glFinish());
    if(!wglDXUnlockObjectsNV(pImpl->hDxDevice, 1, &pImpl->hDxObject)) return;
    
    pImpl->frameValue++;
    pImpl->pDeviceContext->Signal(pImpl->d3d11Fence.Get(), pImpl->frameValue);
    
    if (pImpl->pManifestView) {
        InterlockedExchange64(reinterpret_cast<volatile LONGLONG*>(&pImpl->pManifestView->frameValue), pImpl->frameValue);
        WakeByAddressAll(&pImpl->pManifestView->frameValue);
    }
}
unsigned long ProducerGL::get_pid() const { return pImpl->pid; }

ConsumerGL::ConsumerGL() : pImpl(std::make_unique<Impl>()) {}
ConsumerGL::~ConsumerGL() {
    if (pImpl->hProcess) CloseHandle(pImpl->hProcess);
    if (pImpl->glSemaphore) glDeleteSemaphoresEXT(1, &pImpl->glSemaphore);
}

bool ConsumerGL::wait_for_frame(uint32_t timeout_ms) {
    if (!is_alive()) return false;
    
    UINT64 currentFrame = pImpl->lastSeenFrame;
    WaitOnAddress(&pImpl->currentManifest.frameValue, &currentFrame, sizeof(UINT64), timeout_ms);
    
    std::wstring manifest_name;
    if(!get_manifest_from_pid(pImpl->pid, pImpl->currentManifest, manifest_name)) return false;

    if (pImpl->currentManifest.frameValue > pImpl->lastSeenFrame) {
        pImpl->lastSeenFrame = pImpl->currentManifest.frameValue;
        
        const GLuint textureID = pImpl->privateTexture->pImpl->textureID;
        const GLenum dstLayout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
        
        GL_CHECK(glWaitSemaphoreEXT(pImpl->glSemaphore, 0, nullptr, 1, &textureID, &dstLayout));
        return true;
    }
    return false;
}

bool ConsumerGL::is_alive() const {
    if (!pImpl->hProcess) return true;
    return WaitForSingleObject(pImpl->hProcess, 0) == WAIT_TIMEOUT;
}
std::shared_ptr<TextureGL> ConsumerGL::get_texture() { return pImpl->privateTexture; }
unsigned long ConsumerGL::get_pid() const { return pImpl->pid; }

std::vector<ProducerInfo> discover() {
    std::vector<ProducerInfo> producers;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return producers;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            BroadcastManifest manifest;
            std::wstring manifest_name;
            if (get_manifest_from_pid(pe32.th32ProcessID, manifest, manifest_name)) {
                ProducerInfo info;
                info.pid = pe32.th32ProcessID;
                info.executable_name = pe32.szExeFile;
                
                std::wstring type;
                if (manifest_name.find(L"DirectPortGL") != std::wstring::npos) type = L"OpenGL";
                else if (manifest_name.find(L"D3D12") != std::wstring::npos) type = L"D3D12";
                else type = L"D3D11";
                info.type = type;

                std::wstring textureName(manifest.textureName);
                size_t first = textureName.find(L'_') + 1;
                first = textureName.find(L'_', first) + 1;
                size_t last = textureName.find_last_of(L'_');
                if (last > first) {
                    info.stream_name = textureName.substr(first, last - first);
                }

                producers.push_back(info);
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return producers;
}

}