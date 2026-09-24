#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dcomp.h>
#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kClassName[] = L"DirectPort.DCompSurfaceReceipt";
constexpr UINT kWidth = 520;
constexpr UINT kHeight = 220;
constexpr UINT_PTR kDirtyTimer = 1;

struct State {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionSurface> surface;
    bool toggled = false;
};

HRESULT Draw(State& s, const RECT* dirty) {
    ComPtr<IDXGISurface> dxgiSurface;
    POINT offset{};
    HRESULT hr = s.surface->BeginDraw(dirty, IID_PPV_ARGS(&dxgiSurface), &offset);
    if (FAILED(hr)) return hr;

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> targetBitmap;
    hr = s.d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &props, &targetBitmap);
    if (FAILED(hr)) {
        s.surface->EndDraw();
        return hr;
    }

    s.d2dContext->SetTarget(targetBitmap.Get());
    s.d2dContext->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(offset.x), static_cast<float>(offset.y)));
    s.d2dContext->BeginDraw();

    if (!dirty) {
        s.d2dContext->Clear(D2D1::ColorF(0, 0.0f));

        ComPtr<ID2D1SolidColorBrush> panel;
        s.d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.075f, 0.075f, 0.09f, 0.94f), &panel);
        const auto body = D2D1::RoundedRect(D2D1::RectF(8.0f, 8.0f, 512.0f, 212.0f), 18.0f, 18.0f);
        s.d2dContext->FillRoundedRectangle(body, panel.Get());

        ComPtr<ID2D1SolidColorBrush> edge;
        s.d2dContext->CreateSolidColorBrush(D2D1::ColorF(0.75f, 0.82f, 0.95f, 0.25f), &edge);
        s.d2dContext->DrawRoundedRectangle(body, edge.Get(), 1.0f);
    }

    ComPtr<ID2D1SolidColorBrush> receipt;
    const D2D1_COLOR_F c = s.toggled
        ? D2D1::ColorF(0.20f, 0.75f, 0.95f, 0.95f)
        : D2D1::ColorF(0.35f, 0.45f, 0.95f, 0.95f);
    s.d2dContext->CreateSolidColorBrush(c, &receipt);
    s.d2dContext->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(24.0f, 24.0f, 220.0f, 64.0f), 10.0f, 10.0f),
        receipt.Get());

    hr = s.d2dContext->EndDraw();
    s.d2dContext->SetTransform(D2D1::Matrix3x2F::Identity());
    s.d2dContext->SetTarget(nullptr);
    const HRESULT endHr = s.surface->EndDraw();
    if (FAILED(hr)) return hr;
    return endHr;
}

HRESULT Initialize(State& s) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        requested,
        ARRAYSIZE(requested),
        D3D11_SDK_VERSION,
        &s.d3d,
        &level,
        nullptr);
    if (hr == E_INVALIDARG) {
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            requested + 1,
            ARRAYSIZE(requested) - 1,
            D3D11_SDK_VERSION,
            &s.d3d,
            &level,
            nullptr);
    }
    if (FAILED(hr)) return hr;

    hr = s.d3d.As(&s.dxgi);
    if (FAILED(hr)) return hr;

    D2D1_FACTORY_OPTIONS factoryOptions{};
    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &factoryOptions,
        reinterpret_cast<void**>(s.d2dFactory.GetAddressOf()));
    if (FAILED(hr)) return hr;

    hr = s.d2dFactory->CreateDevice(s.dxgi.Get(), &s.d2dDevice);
    if (FAILED(hr)) return hr;

    hr = s.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s.d2dContext);
    if (FAILED(hr)) return hr;
    s.d2dContext->SetDpi(96.0f, 96.0f);
    s.d2dContext->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    hr = DCompositionCreateDevice2(s.dxgi.Get(), IID_PPV_ARGS(&s.dcomp));
    if (FAILED(hr)) return hr;

    hr = s.dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &s.target);
    if (FAILED(hr)) return hr;

    hr = s.dcomp->CreateVisual(&s.visual);
    if (FAILED(hr)) return hr;

    hr = s.dcomp->CreateSurface(
        kWidth,
        kHeight,
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_ALPHA_MODE_PREMULTIPLIED,
        &s.surface);
    if (FAILED(hr)) return hr;

    hr = s.visual->SetContent(s.surface.Get());
    if (FAILED(hr)) return hr;
    hr = s.target->SetRoot(s.visual.Get());
    if (FAILED(hr)) return hr;

    hr = Draw(s, nullptr);
    if (FAILED(hr)) return hr;
    return s.dcomp->Commit();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_TIMER:
        if (s && wParam == kDirtyTimer) {
            s->toggled = !s->toggled;
            RECT dirty{20, 20, 224, 68};
            if (SUCCEEDED(Draw(*s, &dirty))) s->dcomp->Commit();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kDirtyTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;

    State state;
    state.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
        kClassName,
        L"DirectPort DComp receipt",
        WS_POPUP,
        160,
        160,
        kWidth,
        kHeight,
        nullptr,
        nullptr,
        instance,
        &state);
    if (!state.hwnd) return 3;

    const HRESULT hr = Initialize(state);
    if (FAILED(hr)) {
        DestroyWindow(state.hwnd);
        CoUninitialize();
        return static_cast<int>(hr);
    }

    ShowWindow(state.hwnd, SW_SHOWNOACTIVATE);
    SetTimer(state.hwnd, kDirtyTimer, 900, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    state.surface.Reset();
    state.visual.Reset();
    state.target.Reset();
    if (state.dcomp) state.dcomp->Commit();
    state.dcomp.Reset();
    state.d2dContext.Reset();
    state.d2dDevice.Reset();
    state.d2dFactory.Reset();
    state.dxgi.Reset();
    state.d3d.Reset();
    CoUninitialize();
    return 0;
}
