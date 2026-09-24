#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <shellapi.h>

#include <string>
#include <vector>
#include <algorithm>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

static constexpr DWORD HeartbeatIntervalMs = 5000;
static constexpr UINT  HeartbeatTimeoutMs  = 500;
static constexpr int   HeartbeatMissLimit  = 3;
static constexpr size_t TailLimitBytes      = 32 * 1024;

static bool FileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());

    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring DirectoryOf(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");

    if (slash == std::wstring::npos)
        return L".";

    return path.substr(0, slash);
}

static std::wstring Quote(const std::wstring& value)
{
    std::wstring result = L"\"";

    unsigned backslashes = 0;

    for (wchar_t ch : value)
    {
        if (ch == L'\\')
        {
            backslashes++;
            continue;
        }

        if (ch == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result += L'"';
            backslashes = 0;
            continue;
        }

        result.append(backslashes, L'\\');
        backslashes = 0;
        result += ch;
    }

    result.append(backslashes * 2, L'\\');
    result += L'"';

    return result;
}

static std::wstring GetExePath()
{
    std::vector<wchar_t> buffer(32768);

    const DWORD length = GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size())
    );

    if (length == 0 || length >= buffer.size())
        return L"";

    return std::wstring(buffer.data(), length);
}

static std::wstring GetFullPath(const std::wstring& path)
{
    const DWORD required = GetFullPathNameW(
        path.c_str(),
        0,
        nullptr,
        nullptr
    );

    if (required == 0)
        return path;

    std::vector<wchar_t> buffer(required + 1);

    const DWORD length = GetFullPathNameW(
        path.c_str(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        nullptr
    );

    if (length == 0 || length >= buffer.size())
        return path;

    return std::wstring(buffer.data(), length);
}

static std::wstring FindPwsh()
{
    wchar_t found[32768] = {};

    const DWORD length = SearchPathW(
        nullptr,
        L"pwsh.exe",
        nullptr,
        static_cast<DWORD>(_countof(found)),
        found,
        nullptr
    );

    if (length > 0 && length < _countof(found))
        return std::wstring(found, length);

    wchar_t programFiles[32768] = {};

    const DWORD envLength = GetEnvironmentVariableW(
        L"ProgramFiles",
        programFiles,
        static_cast<DWORD>(_countof(programFiles))
    );

    if (envLength > 0 && envLength < _countof(programFiles))
    {
        std::wstring candidate =
            std::wstring(programFiles) +
            L"\\PowerShell\\7\\pwsh.exe";

        if (FileExists(candidate))
            return candidate;
    }

    return L"";
}

static std::wstring Win32Error(DWORD code)
{
    wchar_t* text = nullptr;

    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&text),
        0,
        nullptr
    );

    std::wstring result;

    if (length && text)
    {
        result.assign(text, length);

        while (!result.empty() &&
               (result.back() == L'\r' ||
                result.back() == L'\n'))
        {
            result.pop_back();
        }

        LocalFree(text);
    }
    else
    {
        result = L"Win32 error " + std::to_wstring(code);
    }

    return result;
}

static int Fail(
    const std::wstring& message,
    DWORD code = 0)
{
    std::wstring text = message;

    if (code != 0)
    {
        text += L"\r\n\r\n";
        text += Win32Error(code);
        text += L"\r\nError code: ";
        text += std::to_wstring(code);
    }

    MessageBoxW(
        nullptr,
        text.c_str(),
        L"SMA",
        MB_OK | MB_ICONERROR | MB_SETFOREGROUND
    );

    return 1;
}


// ---------------------------------------------------------------------
// Bounded output capture
// ---------------------------------------------------------------------

struct TailBuffer
{
    CRITICAL_SECTION Lock;
    std::string Bytes;
};

static void InitTail(TailBuffer& tail)
{
    InitializeCriticalSection(&tail.Lock);
}

static void DestroyTail(TailBuffer& tail)
{
    DeleteCriticalSection(&tail.Lock);
}

static void AppendTail(
    TailBuffer& tail,
    const char* bytes,
    DWORD count)
{
    if (count == 0)
        return;

    EnterCriticalSection(&tail.Lock);

    tail.Bytes.append(bytes, count);

    if (tail.Bytes.size() > TailLimitBytes)
    {
        const size_t remove =
            tail.Bytes.size() - TailLimitBytes;

        tail.Bytes.erase(0, remove);
    }

    LeaveCriticalSection(&tail.Lock);
}

static std::string SnapshotTail(TailBuffer& tail)
{
    EnterCriticalSection(&tail.Lock);

    std::string copy = tail.Bytes;

    LeaveCriticalSection(&tail.Lock);

    return copy;
}

struct PipeReader
{
    HANDLE Pipe;
    TailBuffer* Tail;
};

static DWORD WINAPI ReadPipeThread(void* raw)
{
    PipeReader* reader =
        static_cast<PipeReader*>(raw);

    char buffer[4096];

    for (;;)
    {
        DWORD read = 0;

        const BOOL ok = ReadFile(
            reader->Pipe,
            buffer,
            static_cast<DWORD>(sizeof(buffer)),
            &read,
            nullptr
        );

        if (!ok || read == 0)
            break;

        AppendTail(
            *reader->Tail,
            buffer,
            read
        );
    }

    return 0;
}

static std::wstring DecodeOutput(
    const std::string& bytes)
{
    if (bytes.empty())
        return L"";

    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;

    int required = MultiByteToWideChar(
        codePage,
        flags,
        bytes.data(),
        static_cast<int>(bytes.size()),
        nullptr,
        0
    );

    if (required == 0)
    {
        codePage = CP_ACP;
        flags = 0;

        required = MultiByteToWideChar(
            codePage,
            flags,
            bytes.data(),
            static_cast<int>(bytes.size()),
            nullptr,
            0
        );
    }

    if (required <= 0)
        return L"<output could not be decoded>";

    std::wstring result(required, L'\0');

    MultiByteToWideChar(
        codePage,
        flags,
        bytes.data(),
        static_cast<int>(bytes.size()),
        &result[0],
        required
    );

    return result;
}


// ---------------------------------------------------------------------
// Window heartbeat
// ---------------------------------------------------------------------

struct WindowSearch
{
    DWORD ProcessId;
    HWND Window;
};

static BOOL CALLBACK FindProcessWindow(
    HWND hwnd,
    LPARAM parameter)
{
    WindowSearch* search =
        reinterpret_cast<WindowSearch*>(parameter);

    DWORD processId = 0;

    GetWindowThreadProcessId(
        hwnd,
        &processId
    );

    if (processId != search->ProcessId)
        return TRUE;

    if (!IsWindowVisible(hwnd))
        return TRUE;

    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;

    search->Window = hwnd;

    return FALSE;
}

static HWND FindTopLevelWindow(DWORD processId)
{
    WindowSearch search = {};
    search.ProcessId = processId;

    EnumWindows(
        FindProcessWindow,
        reinterpret_cast<LPARAM>(&search)
    );

    return search.Window;
}

static std::wstring GetWindowTitle(HWND hwnd)
{
    const int length =
        GetWindowTextLengthW(hwnd);

    if (length <= 0)
        return L"";

    std::vector<wchar_t> buffer(
        static_cast<size_t>(length) + 1
    );

    GetWindowTextW(
        hwnd,
        buffer.data(),
        static_cast<int>(buffer.size())
    );

    return std::wstring(buffer.data());
}

static bool IsWindowResponsive(HWND hwnd)
{
    if (!hwnd || !IsWindow(hwnd))
        return false;

    DWORD_PTR ignored = 0;

    const LRESULT result = SendMessageTimeoutW(
        hwnd,
        WM_NULL,
        0,
        0,
        SMTO_ABORTIFHUNG | SMTO_BLOCK,
        HeartbeatTimeoutMs,
        &ignored
    );

    return result != 0;
}


// ---------------------------------------------------------------------
// Native SMA emergency presentation.
//
// Deliberately independent of DirectPort and application code.
// One D3D11/D2D frame, then DWM owns the pixels while this thread idles.
// ---------------------------------------------------------------------

enum SMA_ACCENT_STATE
{
    SMA_ACCENT_DISABLED                    = 0,
    SMA_ACCENT_ENABLE_GRADIENT             = 1,
    SMA_ACCENT_ENABLE_TRANSPARENTGRADIENT  = 2,
    SMA_ACCENT_ENABLE_BLURBEHIND           = 3,
    SMA_ACCENT_ENABLE_ACRYLICBLURBEHIND    = 4
};

struct SMA_ACCENT_POLICY
{
    SMA_ACCENT_STATE AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;
    DWORD AnimationId;
};

struct SMA_WINDOWCOMPOSITIONATTRIBDATA
{
    DWORD Attrib;
    PVOID Data;
    SIZE_T Size;
};

using SmaSetWindowCompositionAttribute =
    BOOL (WINAPI*)(
        HWND,
        SMA_WINDOWCOMPOSITIONATTRIBDATA*
    );


static std::wstring StripTerminalSequences(
    const std::wstring& text)
{
    std::wstring result;
    result.reserve(text.size());

    for (size_t i = 0; i < text.size();)
    {
        if (text[i] != 0x1B)
        {
            result += text[i++];
            continue;
        }

        if (i + 1 >= text.size())
            break;

        // CSI: ESC [ ... final byte
        if (text[i + 1] == L'[')
        {
            i += 2;

            while (i < text.size())
            {
                const wchar_t ch = text[i++];

                if (ch >= 0x40 && ch <= 0x7E)
                    break;
            }

            continue;
        }

        // OSC: ESC ] ... BEL or ESC backslash
        if (text[i + 1] == L']')
        {
            i += 2;

            while (i < text.size())
            {
                if (text[i] == 0x07)
                {
                    ++i;
                    break;
                }

                if (text[i] == 0x1B &&
                    i + 1 < text.size() &&
                    text[i + 1] == L'\\')
                {
                    i += 2;
                    break;
                }

                ++i;
            }

            continue;
        }

        // Unknown two-byte escape.
        i += 2;
    }

    return result;
}


static std::wstring TrimDiagnosticForToast(
    const std::wstring& text)
{
    static constexpr size_t Limit = 6000;
    static constexpr size_t Head  = 2800;
    static constexpr size_t Tail  = 2800;

    if (text.size() <= Limit)
        return text;

    return
        text.substr(0, Head) +
        L"\r\n\r\n"
        L"[… diagnostic clipped …]"
        L"\r\n\r\n" +
        text.substr(text.size() - Tail);
}


static LRESULT CALLBACK SmaFailureWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    switch (message)
    {
        case WM_LBUTTONUP:
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
    }

    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam
    );
}


static bool ShowSmaFailureToast(
    const std::wstring& title,
    const std::wstring& message)
{
    HINSTANCE instance =
        GetModuleHandleW(nullptr);

    static constexpr wchar_t ClassName[] =
        L"SMA.FailureToast.v1";

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = SmaFailureWindowProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = ClassName;

    if (!RegisterClassW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }


    RECT work = {};

    if (!SystemParametersInfoW(
            SPI_GETWORKAREA,
            0,
            &work,
            0))
    {
        return false;
    }

    const int availableWidth =
        static_cast<int>(work.right - work.left) - 40;

    const int availableHeight =
        static_cast<int>(work.bottom - work.top) - 40;

    const int width =
        availableWidth < 760
            ? availableWidth
            : 760;

    const int height =
        availableHeight < 460
            ? availableHeight
            : 460;

    const int x =
        work.right - width - 20;

    const int y =
        work.bottom - height - 20;


    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST |
        WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE,

        ClassName,
        L"SMA",

        WS_POPUP |
        WS_VISIBLE,

        x,
        y,
        width,
        height,

        nullptr,
        nullptr,
        instance,
        nullptr
    );

    if (!hwnd)
        return false;


    // Ask DWM for the same native blur primitive as the prior.
    HMODULE user32 =
        GetModuleHandleW(L"user32.dll");

    if (user32)
    {
        auto setComposition =
            reinterpret_cast<SmaSetWindowCompositionAttribute>(
                GetProcAddress(
                    user32,
                    "SetWindowCompositionAttribute"
                )
            );

        if (setComposition)
        {
            SMA_ACCENT_POLICY policy = {
                SMA_ACCENT_ENABLE_BLURBEHIND,
                0,
                0,
                0
            };

            SMA_WINDOWCOMPOSITIONATTRIBDATA data = {
                19, // WCA_ACCENT_POLICY
                &policy,
                sizeof(policy)
            };

            setComposition(
                hwnd,
                &data
            );
        }
    }


    // D3D11: tiny independent emergency renderer.
    ComPtr<ID3D11Device> d3dDevice;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3dDevice,
        nullptr,
        nullptr
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    ComPtr<IDXGIFactory4> factory;

    hr = CreateDXGIFactory1(
        IID_PPV_ARGS(&factory)
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width              = width;
    desc.Height             = height;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = 2;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode          = DXGI_ALPHA_MODE_UNSPECIFIED;


    ComPtr<IDXGISwapChain1> swapChain;

    hr = factory->CreateSwapChainForHwnd(
        d3dDevice.Get(),
        hwnd,
        &desc,
        nullptr,
        nullptr,
        &swapChain
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    factory->MakeWindowAssociation(
        hwnd,
        DXGI_MWA_NO_ALT_ENTER
    );


    ComPtr<IDXGIDevice> dxgiDevice;

    hr = d3dDevice.As(&dxgiDevice);

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    ComPtr<ID2D1Factory1> d2dFactory;

    hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        d2dFactory.GetAddressOf()
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    ComPtr<ID2D1Device> d2dDevice;

    hr = d2dFactory->CreateDevice(
        dxgiDevice.Get(),
        &d2dDevice
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    ComPtr<ID2D1DeviceContext> context;

    hr = d2dDevice->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &context
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    ComPtr<IDWriteFactory> writeFactory;

    hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(
            writeFactory.GetAddressOf()
        )
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    ComPtr<IDWriteTextFormat> titleFormat;
    ComPtr<IDWriteTextFormat> bodyFormat;

    hr = writeFactory->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        22.0f,
        L"en-us",
        &titleFormat
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }

    hr = writeFactory->CreateTextFormat(
        L"Consolas",
        nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.5f,
        L"en-us",
        &bodyFormat
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }

    bodyFormat->SetWordWrapping(
        DWRITE_WORD_WRAPPING_WRAP
    );


    ComPtr<IDXGISurface> backBuffer;

    hr = swapChain->GetBuffer(
        0,
        IID_PPV_ARGS(&backBuffer)
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    const D2D1_BITMAP_PROPERTIES1 properties =
        D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET |
            D2D1_BITMAP_OPTIONS_CANNOT_DRAW,

            D2D1::PixelFormat(
                DXGI_FORMAT_B8G8R8A8_UNORM,
                D2D1_ALPHA_MODE_PREMULTIPLIED
            )
        );


    ComPtr<ID2D1Bitmap1> target;

    hr = context->CreateBitmapFromDxgiSurface(
        backBuffer.Get(),
        &properties,
        &target
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }

    context->SetTarget(
        target.Get()
    );


    ComPtr<ID2D1SolidColorBrush> titleBrush;
    ComPtr<ID2D1SolidColorBrush> bodyBrush;
    ComPtr<ID2D1SolidColorBrush> accentBrush;
    ComPtr<ID2D1SolidColorBrush> errorBrush;

    context->CreateSolidColorBrush(
        D2D1::ColorF(
            1.0f,
            1.0f,
            1.0f,
            0.98f
        ),
        &titleBrush
    );

    context->CreateSolidColorBrush(
        D2D1::ColorF(
            0.90f,
            0.90f,
            0.92f,
            0.96f
        ),
        &bodyBrush
    );

    // Fluent accent: #60cdff
    context->CreateSolidColorBrush(
        D2D1::ColorF(
            0x60CDFF,
            1.0f
        ),
        &accentBrush
    );

    context->CreateSolidColorBrush(
        D2D1::ColorF(
            0xE81123,
            1.0f
        ),
        &errorBrush
    );


    const std::wstring body =
        TrimDiagnosticForToast(message);


    // Render exactly one frame.
    context->BeginDraw();

    context->Clear(
        D2D1::ColorF(
            0.055f,
            0.055f,
            0.065f,
            0.78f
        )
    );

    context->SetTextAntialiasMode(
        D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE
    );


    // Thin SMA accent and failure rail.
    context->FillRectangle(
        D2D1::RectF(
            0.0f,
            0.0f,
            static_cast<float>(width),
            3.0f
        ),
        accentBrush.Get()
    );

    context->FillRectangle(
        D2D1::RectF(
            0.0f,
            3.0f,
            4.0f,
            static_cast<float>(height)
        ),
        errorBrush.Get()
    );


    context->DrawText(
        title.c_str(),
        static_cast<UINT32>(title.length()),
        titleFormat.Get(),
        D2D1::RectF(
            24.0f,
            22.0f,
            static_cast<float>(width) - 24.0f,
            62.0f
        ),
        titleBrush.Get()
    );


    context->DrawText(
        body.c_str(),
        static_cast<UINT32>(body.length()),
        bodyFormat.Get(),
        D2D1::RectF(
            24.0f,
            76.0f,
            static_cast<float>(width) - 24.0f,
            static_cast<float>(height) - 24.0f
        ),
        bodyBrush.Get()
    );


    hr = context->EndDraw();

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }

    hr = swapChain->Present(
        1,
        0
    );

    if (FAILED(hr))
    {
        DestroyWindow(hwnd);
        return false;
    }


    // John Travolta.
    //
    // Static frame. DWM does the work. We only keep Windows responsive.
    // Click anywhere to dismiss; otherwise disappear after ten seconds.
    const DWORD started =
        GetTickCount();

    MSG msg = {};

    while (IsWindow(hwnd) &&
           GetTickCount() - started < 10000)
    {
        while (PeekMessageW(
            &msg,
            nullptr,
            0,
            0,
            PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Sleep(16);
    }

    if (IsWindow(hwnd))
        DestroyWindow(hwnd);

    return true;
}

// ---------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------

static std::wstring HexHandle(HWND hwnd)
{
    wchar_t text[32] = {};

    swprintf_s(
        text,
        L"0x%p",
        hwnd
    );

    return text;
}

static std::wstring BuildCapturedOutput(
    TailBuffer& stdoutTail,
    TailBuffer& stderrTail)
{
    const std::wstring out =
        StripTerminalSequences(DecodeOutput(SnapshotTail(stdoutTail)));

    const std::wstring err =
        StripTerminalSequences(DecodeOutput(SnapshotTail(stderrTail)));

    std::wstring result;

    if (!err.empty())
    {
        result += L"\r\n\r\n--- stderr ---\r\n";
        result += err;
    }

    if (!out.empty())
    {
        result += L"\r\n\r\n--- stdout ---\r\n";
        result += out;
    }

    if (result.empty())
    {
        result =
            L"\r\n\r\nNo captured PowerShell output.";
    }

    return result;
}

static int ReportHang(
    DWORD runtimePid,
    HWND hwnd,
    int misses,
    TailBuffer& stdoutTail,
    TailBuffer& stderrTail)
{
    std::wstring text;

    text += L"The SMA application is not responding.\r\n\r\n";

    text += L"Host PID: ";
    text += std::to_wstring(GetCurrentProcessId());

    text += L"\r\nRuntime PID: ";
    text += std::to_wstring(runtimePid);

    text += L"\r\nWindow: ";
    text += HexHandle(hwnd);

    const std::wstring title =
        GetWindowTitle(hwnd);

    if (!title.empty())
    {
        text += L"\r\nTitle: ";
        text += title;
    }

    text += L"\r\nProcess alive: Yes";
    text += L"\r\nWindow responsive: No";

    text += L"\r\nHeartbeat misses: ";
    text += std::to_wstring(misses);

    text += BuildCapturedOutput(
        stdoutTail,
        stderrTail
    );

    text +=
        L"\r\n\r\nRetry = keep waiting"
        L"\r\nCancel = terminate runtime";

    return MessageBoxW(
        nullptr,
        text.c_str(),
        L"SMA application not responding",
        MB_RETRYCANCEL |
        MB_ICONWARNING |
        MB_SETFOREGROUND
    );
}

static void ReportFailure(
    DWORD runtimePid,
    DWORD exitCode,
    TailBuffer& stdoutTail,
    TailBuffer& stderrTail)
{
    std::wstring text;

    text += L"Host PID     ";
    text += std::to_wstring(GetCurrentProcessId());

    text += L"\r\nRuntime PID  ";
    text += std::to_wstring(runtimePid);

    text += L"\r\nExit code    ";
    text += std::to_wstring(exitCode);

    text += BuildCapturedOutput(
        stdoutTail,
        stderrTail
    );

    text = StripTerminalSequences(text);

    if (!ShowSmaFailureToast(
            L"SEXE_APP_FAILED",
            text))
    {
        // Emergency renderer itself failed. Never lose the diagnostic.
        MessageBoxW(
            nullptr,
            text.c_str(),
            L"SEXE_APP_FAILED",
            MB_OK |
            MB_ICONERROR |
            MB_SETFOREGROUND
        );
    }
}


// ---------------------------------------------------------------------
// SMA.exe
// ---------------------------------------------------------------------

int WINAPI wWinMain(
    HINSTANCE,
    HINSTANCE,
    PWSTR,
    int)
{
    int argc = 0;

    LPWSTR* argv = CommandLineToArgvW(
        GetCommandLineW(),
        &argc
    );

    if (!argv)
        return Fail(
            L"Could not parse command line.",
            GetLastError()
        );

    if (argc != 2)
    {
        LocalFree(argv);

        return Fail(
            L"Usage:\r\n\r\n"
            L"SMA.exe <application.Sexe>"
        );
    }

    const std::wstring package =
        GetFullPath(argv[1]);

    LocalFree(argv);

    if (!FileExists(package))
    {
        return Fail(
            L"SMA application not found:\r\n\r\n" +
            package
        );
    }

    const std::wstring exePath =
        GetExePath();

    if (exePath.empty())
    {
        return Fail(
            L"Could not resolve SMA.exe path.",
            GetLastError()
        );
    }

    const std::wstring exeDir =
        DirectoryOf(exePath);

    const std::wstring packageDir =
        DirectoryOf(package);

    std::wstring handler =
        packageDir +
        L"\\Sexe\\Invoke-SexeFile.ps1";

    if (!FileExists(handler))
    {
        handler =
            exeDir +
            L"\\Sexe\\Invoke-SexeFile.ps1";
    }

    if (!FileExists(handler))
    {
        return Fail(
            L"SEXE admission script was not found.\r\n\r\n"
            L"Expected one of:\r\n\r\n" +
            packageDir +
            L"\\Sexe\\Invoke-SexeFile.ps1\r\n\r\n" +
            exeDir +
            L"\\Sexe\\Invoke-SexeFile.ps1"
        );
    }

    const std::wstring pwsh =
        FindPwsh();

    if (pwsh.empty())
    {
        return Fail(
            L"pwsh.exe was not found."
        );
    }


    // -------------------------------------------------------------
    // stdout / stderr pipes
    // -------------------------------------------------------------

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;

    HANDLE stderrRead = nullptr;
    HANDLE stderrWrite = nullptr;

    if (!CreatePipe(
            &stdoutRead,
            &stdoutWrite,
            &security,
            0))
    {
        return Fail(
            L"Could not create stdout pipe.",
            GetLastError()
        );
    }

    if (!SetHandleInformation(
            stdoutRead,
            HANDLE_FLAG_INHERIT,
            0))
    {
        const DWORD error = GetLastError();

        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);

        return Fail(
            L"Could not configure stdout pipe.",
            error
        );
    }

    if (!CreatePipe(
            &stderrRead,
            &stderrWrite,
            &security,
            0))
    {
        const DWORD error = GetLastError();

        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);

        return Fail(
            L"Could not create stderr pipe.",
            error
        );
    }

    if (!SetHandleInformation(
            stderrRead,
            HANDLE_FLAG_INHERIT,
            0))
    {
        const DWORD error = GetLastError();

        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);

        return Fail(
            L"Could not configure stderr pipe.",
            error
        );
    }

    HANDLE nullInput = CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (nullInput == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();

        CloseHandle(stdoutRead);
        CloseHandle(stdoutWrite);
        CloseHandle(stderrRead);
        CloseHandle(stderrWrite);

        return Fail(
            L"Could not open NUL for child stdin.",
            error
        );
    }


    // -------------------------------------------------------------
    // Existing phase-0 hidden PowerShell admission path
    // -------------------------------------------------------------

    std::wstring command;

    command += Quote(pwsh);
    command += L" -NoLogo";
    command += L" -NoProfile";
    command += L" -NonInteractive";
    command += L" -ExecutionPolicy Bypass";
    command += L" -File ";
    command += Quote(handler);
    command += L" ";
    command += Quote(package);

    std::vector<wchar_t> commandLine(
        command.begin(),
        command.end()
    );

    commandLine.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);

    startup.dwFlags =
        STARTF_USESTDHANDLES;

    startup.hStdInput  = nullInput;
    startup.hStdOutput = stdoutWrite;
    startup.hStdError  = stderrWrite;

    PROCESS_INFORMATION process = {};

    const BOOL launched = CreateProcessW(
        pwsh.c_str(),
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        packageDir.c_str(),
        &startup,
        &process
    );

    const DWORD launchError =
        launched ? ERROR_SUCCESS : GetLastError();

    CloseHandle(stdoutWrite);
    CloseHandle(stderrWrite);
    CloseHandle(nullInput);

    if (!launched)
    {
        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);

        return Fail(
            L"Could not launch SMA runtime.",
            launchError
        );
    }

    CloseHandle(process.hThread);


    // -------------------------------------------------------------
    // Continuous bounded pipe drainage.
    //
    // These threads normally spend their entire lives asleep inside
    // ReadFile(). They exist so a chatty child can never fill a pipe
    // and deadlock itself.
    // -------------------------------------------------------------

    TailBuffer stdoutTail;
    TailBuffer stderrTail;

    InitTail(stdoutTail);
    InitTail(stderrTail);

    PipeReader stdoutReader = {
        stdoutRead,
        &stdoutTail
    };

    PipeReader stderrReader = {
        stderrRead,
        &stderrTail
    };

    HANDLE stdoutThread = CreateThread(
        nullptr,
        0,
        ReadPipeThread,
        &stdoutReader,
        0,
        nullptr
    );

    HANDLE stderrThread = CreateThread(
        nullptr,
        0,
        ReadPipeThread,
        &stderrReader,
        0,
        nullptr
    );

    if (!stdoutThread || !stderrThread)
    {
        const DWORD error = GetLastError();

        TerminateProcess(
            process.hProcess,
            1
        );

        WaitForSingleObject(
            process.hProcess,
            INFINITE
        );

        if (stdoutThread)
        {
            WaitForSingleObject(
                stdoutThread,
                INFINITE
            );

            CloseHandle(stdoutThread);
        }

        if (stderrThread)
        {
            WaitForSingleObject(
                stderrThread,
                INFINITE
            );

            CloseHandle(stderrThread);
        }

        CloseHandle(stdoutRead);
        CloseHandle(stderrRead);
        CloseHandle(process.hProcess);

        DestroyTail(stdoutTail);
        DestroyTail(stderrTail);

        return Fail(
            L"Could not start diagnostic pipe reader.",
            error
        );
    }


    // -------------------------------------------------------------
    // John Travolta loop.
    //
    // Wait five seconds doing nothing.
    //
    // If the runtime is still alive, locate its real top-level HWND.
    // Once one exists, ask Windows whether that thread is servicing
    // its queue. No window is NOT a failure: SMA applications may be
    // legitimately headless.
    // -------------------------------------------------------------

    int heartbeatMisses = 0;

    for (;;)
    {
        const DWORD wait = WaitForSingleObject(
            process.hProcess,
            HeartbeatIntervalMs
        );

        if (wait == WAIT_OBJECT_0)
            break;

        if (wait == WAIT_FAILED)
        {
            const DWORD error = GetLastError();

            TerminateProcess(
                process.hProcess,
                1
            );

            WaitForSingleObject(
                process.hProcess,
                INFINITE
            );

            ReportFailure(
                process.dwProcessId,
                error,
                stdoutTail,
                stderrTail
            );

            break;
        }

        HWND hwnd =
            FindTopLevelWindow(
                process.dwProcessId
            );

        if (!hwnd)
        {
            heartbeatMisses = 0;
            continue;
        }

        if (IsWindowResponsive(hwnd))
        {
            heartbeatMisses = 0;
            continue;
        }

        heartbeatMisses++;

        if (heartbeatMisses < HeartbeatMissLimit)
            continue;

        const int choice = ReportHang(
            process.dwProcessId,
            hwnd,
            heartbeatMisses,
            stdoutTail,
            stderrTail
        );

        if (choice == IDCANCEL)
        {
            TerminateProcess(
                process.hProcess,
                0xDEAD
            );

            WaitForSingleObject(
                process.hProcess,
                INFINITE
            );

            break;
        }

        // User chose Retry. Give the application another clean window.
        heartbeatMisses = 0;
    }


    // -------------------------------------------------------------
    // Runtime is gone. Closing our process handle does not close the
    // pipe read ends yet; the readers drain whatever bytes remained
    // and terminate naturally when they see EOF.
    // -------------------------------------------------------------

    DWORD exitCode = 1;

    GetExitCodeProcess(
        process.hProcess,
        &exitCode
    );

    WaitForSingleObject(
        stdoutThread,
        INFINITE
    );

    WaitForSingleObject(
        stderrThread,
        INFINITE
    );

    CloseHandle(stdoutThread);
    CloseHandle(stderrThread);

    CloseHandle(stdoutRead);
    CloseHandle(stderrRead);

    CloseHandle(process.hProcess);

    if (exitCode != 0 &&
        exitCode != 0xDEAD)
    {
        ReportFailure(
            process.dwProcessId,
            exitCode,
            stdoutTail,
            stderrTail
        );
    }

    DestroyTail(stdoutTail);
    DestroyTail(stderrTail);

    return static_cast<int>(exitCode);
}
