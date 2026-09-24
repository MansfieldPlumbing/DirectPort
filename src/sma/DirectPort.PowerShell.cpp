// DirectPort.PowerShell -- .NET 11 / PowerShell managed D3D12 seam.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <intrin.h>
#include <vcclr.h>

#include "directport.h"
#include "DirectPort.Shader.Native.h"
#include "DirectPort.Console.Native.h"
#include "DirectPort.Canvas2D.Native.h"

using namespace System;
using namespace System::Runtime::InteropServices;

#pragma managed(push, off)
static void dp12_publish_store_fence() {
    _mm_sfence();
}
#pragma managed(pop)

namespace DirectPort::PowerShell {

    public enum class PixelFormat : int {
        Bgra8   = DP_FORMAT_VIDEO,
        Float32 = DP_FORMAT_FLOAT,
        Float16 = DP_FORMAT_HALF,
        UInt32  = DP_FORMAT_RAW_32BIT
    };

    public enum class MemoryLayout : int {
        CpuRowMajor = 0,
        GpuOptimal = 1
    };

    public enum class CanvasCursor : int {
        Arrow   = DP2D_CURSOR_ARROW,
        Hand    = DP2D_CURSOR_HAND,
        IBeam   = DP2D_CURSOR_IBEAM,
        SizeWE  = DP2D_CURSOR_SIZEWE,
        SizeNS  = DP2D_CURSOR_SIZENS,
        SizeAll = DP2D_CURSOR_SIZEALL
    };

    public enum class CanvasTextAlign : int {
        Leading  = 0,
        Center   = 1,
        Trailing = 2
    };

    public ref class Device abstract sealed {
    public:
        static bool Initialize() {
            return dp12_init();
        }

        static void Shutdown() {
            dp12_shutdown();
        }

        static property bool IsUma {
            bool get() { return dp12_is_uma(); }
        }

        static property Int64 AdapterLuid {
            Int64 get() {
                int64_t luid = 0;
                if (!dp12_get_adapter_luid(&luid))
                    throw gcnew InvalidOperationException("The D3D12 device is not initialized.");
                return luid;
            }
        }

        static property int LastHResult {
            int get() { return dp12_last_hresult(); }
        }
    };

    public ref class SharedResource sealed : IDisposable {
    private:
        IntPtr _native;
        IntPtr _mapped;
        UInt32 _width;
        UInt32 _height;
        UInt32 _rowPitch;
        UInt32 _bytesPerPixel;
        PixelFormat _format;
        MemoryLayout _layout;
        String^ _textureName;
        String^ _fenceName;
        bool _disposed;

        DP_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("SharedResource");
            return _native.ToPointer();
        }

        void EnsureMapped() {
            if (_layout != MemoryLayout::CpuRowMajor)
                throw gcnew InvalidOperationException("GpuOptimal resources are not CPU-mappable.");
            if (_mapped != IntPtr::Zero)
                return;

            uint32_t pitch = 0;
            void* pointer = dp12_map_memory(Handle(), &pitch);
            if (!pointer)
                throw gcnew InvalidOperationException("ID3D12Resource::Map returned null.");
            if ((pitch & 255u) != 0)
                throw gcnew InvalidOperationException("DirectPort returned a row pitch that is not 256-byte aligned.");

            _mapped = IntPtr(pointer);
            _rowPitch = pitch;
        }

    public:
        SharedResource(
            UInt32 width,
            UInt32 height,
            PixelFormat format,
            MemoryLayout layout,
            String^ textureName,
            String^ fenceName)
            : _native(IntPtr::Zero), _mapped(IntPtr::Zero), _width(width),
              _height(height), _rowPitch(0), _format(format), _layout(layout),
              _textureName(textureName), _fenceName(fenceName), _disposed(false) {

            if (width == 0 || height == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width and height must be non-zero.");
            if (String::IsNullOrWhiteSpace(textureName))
                throw gcnew ArgumentException("A shared texture name is required.", "textureName");
            if (String::IsNullOrWhiteSpace(fenceName))
                throw gcnew ArgumentException("A shared fence name is required.", "fenceName");

            _bytesPerPixel = (format == PixelFormat::Float16) ? 2u : 4u;

            if (!dp12_init())
                throw gcnew InvalidOperationException("D3D12CreateDevice failed.");

            pin_ptr<const wchar_t> textureChars = PtrToStringChars(textureName);
            pin_ptr<const wchar_t> fenceChars = PtrToStringChars(fenceName);
            DP_HANDLE native = dp12_create_shared_resource(
                width,
                height,
                static_cast<DP_FORMAT>(format),
                layout == MemoryLayout::CpuRowMajor,
                textureChars,
                fenceChars);

            if (!native) {
                int hr = dp12_last_hresult();
                throw gcnew COMException(
                    String::Format("D3D12 shared-resource creation failed (HRESULT 0x{0:X8}).", hr),
                    hr);
            }

            _native = IntPtr(native);
            if (layout == MemoryLayout::CpuRowMajor)
                EnsureMapped();
        }

        ~SharedResource() {
            this->!SharedResource();
            GC::SuppressFinalize(this);
        }

        !SharedResource() {
            if (_disposed)
                return;
            if (_native != IntPtr::Zero) {
                if (_mapped != IntPtr::Zero)
                    dp12_unmap_memory(_native.ToPointer());
                dp12_close(_native.ToPointer());
            }
            _mapped = IntPtr::Zero;
            _native = IntPtr::Zero;
            _disposed = true;
        }

        property UInt32 Width { UInt32 get() { return _width; } }
        property UInt32 Height { UInt32 get() { return _height; } }
        property UInt32 BytesPerPixel { UInt32 get() { return _bytesPerPixel; } }
        property UInt32 RowPitch {
            UInt32 get() {
                if (_layout == MemoryLayout::CpuRowMajor)
                    EnsureMapped();
                return _rowPitch;
            }
        }
        property PixelFormat Format { PixelFormat get() { return _format; } }
        property MemoryLayout Layout { MemoryLayout get() { return _layout; } }
        property String^ TextureName { String^ get() { return _textureName; } }
        property String^ FenceName { String^ get() { return _fenceName; } }
        property IntPtr NativeHandle { IntPtr get() { Handle(); return _native; } }
        property IntPtr MappedAddress {
            IntPtr get() { EnsureMapped(); return _mapped; }
        }
        property IntPtr SharedTextureHandle {
            IntPtr get() { return IntPtr(dp12_get_resource_handle(Handle())); }
        }
        property IntPtr SharedFenceHandle {
            IntPtr get() { return IntPtr(dp12_get_fence_handle(Handle())); }
        }
        property UInt64 CompletedValue {
            UInt64 get() { return dp12_get_completed_value(Handle()); }
        }

        void WriteRows(array<Byte>^ source, UInt32 sourceRowPitch, UInt32 rowCount) {
            if (source == nullptr)
                throw gcnew ArgumentNullException("source");
            EnsureMapped();
            if (rowCount > _height)
                throw gcnew ArgumentOutOfRangeException("rowCount");

            UInt32 rowBytes = _width * _bytesPerPixel;
            if (sourceRowPitch < rowBytes)
                throw gcnew ArgumentOutOfRangeException("sourceRowPitch", "Source pitch is smaller than one logical row.");
            UInt64 required = static_cast<UInt64>(sourceRowPitch) * rowCount;
            if (required > static_cast<UInt64>(source->LongLength))
                throw gcnew ArgumentException("The source array does not contain every requested row.", "source");

            pin_ptr<Byte> src = &source[0];
            Byte* dst = static_cast<Byte*>(_mapped.ToPointer());
            for (UInt32 y = 0; y < rowCount; ++y)
                memcpy(dst + static_cast<size_t>(y) * _rowPitch,
                       src + static_cast<size_t>(y) * sourceRowPitch,
                       rowBytes);

            dp12_publish_store_fence();
        }

        void WriteCells(array<UInt32>^ cells) {
            if (_format != PixelFormat::UInt32)
                throw gcnew InvalidOperationException("WriteCells requires an UInt32 resource.");
            if (cells == nullptr)
                throw gcnew ArgumentNullException("cells");
            UInt64 required = static_cast<UInt64>(_width) * _height;
            if (static_cast<UInt64>(cells->LongLength) < required)
                throw gcnew ArgumentException("The cell array is smaller than width * height.", "cells");

            EnsureMapped();
            pin_ptr<UInt32> src = &cells[0];
            Byte* dst = static_cast<Byte*>(_mapped.ToPointer());
            size_t rowBytes = static_cast<size_t>(_width) * sizeof(UInt32);
            for (UInt32 y = 0; y < _height; ++y)
                memcpy(dst + static_cast<size_t>(y) * _rowPitch,
                       src + static_cast<size_t>(y) * _width,
                       rowBytes);
            dp12_publish_store_fence();
        }

        void Signal(UInt64 value) {
            dp12_signal_fence(Handle(), value);
        }

        void PublishCells(array<UInt32>^ cells, UInt64 value) {
            WriteCells(cells);
            Signal(value);
        }

        void CpuWait(UInt64 value) {
            dp12_cpu_wait(Handle(), value);
        }
    };

    public ref class FullscreenShaderProducer sealed : IDisposable {
    private:
        IntPtr _native;
        UInt32 _width;
        UInt32 _height;
        UInt32 _constantCapacity;
        PixelFormat _format;
        String^ _textureName;
        String^ _fenceName;
        bool _disposed;

        DPS_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("FullscreenShaderProducer");
            return _native.ToPointer();
        }

    public:
        FullscreenShaderProducer(
            UInt32 width,
            UInt32 height,
            PixelFormat format,
            String^ textureName,
            String^ fenceName,
            String^ hlsl,
            String^ vertexEntry,
            String^ pixelEntry,
            UInt32 constantCapacity)
            : _native(IntPtr::Zero), _width(width), _height(height),
              _constantCapacity(constantCapacity), _format(format),
              _textureName(textureName), _fenceName(fenceName), _disposed(false) {

            if (width == 0 || height == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width and height must be non-zero.");
            if (String::IsNullOrWhiteSpace(textureName) || String::IsNullOrWhiteSpace(fenceName))
                throw gcnew ArgumentException("Shared texture and fence names are required.");
            if (String::IsNullOrWhiteSpace(hlsl))
                throw gcnew ArgumentException("HLSL source is required.", "hlsl");
            if (String::IsNullOrWhiteSpace(vertexEntry) || String::IsNullOrWhiteSpace(pixelEntry))
                throw gcnew ArgumentException("Vertex and pixel entry points are required.");

            IntPtr textureChars = Marshal::StringToHGlobalUni(textureName);
            IntPtr fenceChars = Marshal::StringToHGlobalUni(fenceName);
            IntPtr hlslChars = Marshal::StringToHGlobalAnsi(hlsl);
            IntPtr vertexChars = Marshal::StringToHGlobalAnsi(vertexEntry);
            IntPtr pixelChars = Marshal::StringToHGlobalAnsi(pixelEntry);

            DPS_HANDLE native = dps_create(
                width,
                height,
                static_cast<int32_t>(format),
                static_cast<const wchar_t*>(textureChars.ToPointer()),
                static_cast<const wchar_t*>(fenceChars.ToPointer()),
                static_cast<const char*>(hlslChars.ToPointer()),
                static_cast<const char*>(vertexChars.ToPointer()),
                static_cast<const char*>(pixelChars.ToPointer()),
                constantCapacity);

            Marshal::FreeHGlobal(textureChars);
            Marshal::FreeHGlobal(fenceChars);
            Marshal::FreeHGlobal(hlslChars);
            Marshal::FreeHGlobal(vertexChars);
            Marshal::FreeHGlobal(pixelChars);

            if (!native)
                throw gcnew InvalidOperationException(gcnew String(dps_last_error()));
            _native = IntPtr(native);
        }

        ~FullscreenShaderProducer() {
            this->!FullscreenShaderProducer();
            GC::SuppressFinalize(this);
        }

        !FullscreenShaderProducer() {
            if (_disposed)
                return;
            if (_native != IntPtr::Zero)
                dps_close(_native.ToPointer());
            _native = IntPtr::Zero;
            _disposed = true;
        }

        property UInt32 Width { UInt32 get() { return _width; } }
        property UInt32 Height { UInt32 get() { return _height; } }
        property UInt32 ConstantCapacity { UInt32 get() { return _constantCapacity; } }
        property PixelFormat Format { PixelFormat get() { return _format; } }
        property String^ TextureName { String^ get() { return _textureName; } }
        property String^ FenceName { String^ get() { return _fenceName; } }
        property IntPtr SharedTextureHandle {
            IntPtr get() { return IntPtr(dps_get_texture_handle(Handle())); }
        }
        property IntPtr SharedFenceHandle {
            IntPtr get() { return IntPtr(dps_get_fence_handle(Handle())); }
        }
        property IntPtr DevicePointer {
            IntPtr get() { return IntPtr(dps_get_device_ptr(Handle())); }
        }
        property IntPtr ResourcePointer {
            IntPtr get() { return IntPtr(dps_get_resource_ptr(Handle())); }
        }
        property Int64 AdapterLuid {
            Int64 get() { return dps_get_adapter_luid(Handle()); }
        }
        property UInt64 CompletedValue {
            UInt64 get() { return dps_get_completed_value(Handle()); }
        }

        bool TryDraw(array<Byte>^ constants, UInt64 publishValue) {
            if (constants == nullptr || constants->Length == 0)
                return dps_try_draw(Handle(), nullptr, 0, publishValue);
            if (static_cast<UInt32>(constants->Length) > _constantCapacity)
                throw gcnew ArgumentException("Constant payload exceeds ConstantCapacity.", "constants");
            pin_ptr<Byte> bytes = &constants[0];
            return dps_try_draw(Handle(), bytes, constants->Length, publishValue);
        }

        bool TryDrawFloats(array<Single>^ constants, UInt64 publishValue) {
            if (constants == nullptr || constants->Length == 0)
                return dps_try_draw(Handle(), nullptr, 0, publishValue);
            UInt32 byteCount = static_cast<UInt32>(constants->Length * sizeof(float));
            if (byteCount > _constantCapacity)
                throw gcnew ArgumentException("Constant payload exceeds ConstantCapacity.", "constants");
            pin_ptr<Single> values = &constants[0];
            return dps_try_draw(Handle(), values, byteCount, publishValue);
        }
    };

    public value struct ConsoleWindowState {
        bool Alive;
        Int32 Width;
        Int32 Height;
        Int32 MouseX;
        Int32 MouseY;
        bool LeftDown;
        Int32 WheelDelta;
        Int32 KeyCode;
        UInt64 ResizeSerial;
    };

    public ref class GpuConsole sealed : IDisposable {
    private:
        IntPtr _native;
        UInt32 _width;
        UInt32 _height;
        UInt32 _maxCells;
        String^ _textureName;
        String^ _fenceName;
        bool _disposed;

        DPC_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("GpuConsole");
            return _native.ToPointer();
        }

    public:
        GpuConsole(
            UInt32 width,
            UInt32 height,
            UInt32 maxCells,
            String^ textureName,
            String^ fenceName,
            String^ atlasPng,
            String^ metricsJson)
            : _native(IntPtr::Zero), _width(width), _height(height), _maxCells(maxCells),
              _textureName(textureName), _fenceName(fenceName), _disposed(false) {
            if (width == 0 || height == 0 || maxCells == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width, height, and maxCells must be non-zero.");
            if (String::IsNullOrWhiteSpace(textureName) || String::IsNullOrWhiteSpace(fenceName))
                throw gcnew ArgumentException("Shared texture and fence names are required.");
            if (String::IsNullOrWhiteSpace(atlasPng) || String::IsNullOrWhiteSpace(metricsJson))
                throw gcnew ArgumentException("MSDF atlas PNG and metrics JSON paths are required.");

            IntPtr textureChars = Marshal::StringToHGlobalUni(textureName);
            IntPtr fenceChars = Marshal::StringToHGlobalUni(fenceName);
            IntPtr atlasChars = Marshal::StringToHGlobalUni(atlasPng);
            IntPtr metricsChars = Marshal::StringToHGlobalUni(metricsJson);
            DPC_HANDLE native = dpc_create(
                width, height, maxCells,
                static_cast<const wchar_t*>(textureChars.ToPointer()),
                static_cast<const wchar_t*>(fenceChars.ToPointer()),
                static_cast<const wchar_t*>(atlasChars.ToPointer()),
                static_cast<const wchar_t*>(metricsChars.ToPointer()));
            Marshal::FreeHGlobal(textureChars);
            Marshal::FreeHGlobal(fenceChars);
            Marshal::FreeHGlobal(atlasChars);
            Marshal::FreeHGlobal(metricsChars);
            if (!native)
                throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
            _native = IntPtr(native);
        }

        ~GpuConsole() {
            this->!GpuConsole();
            GC::SuppressFinalize(this);
        }

        !GpuConsole() {
            if (_disposed) return;
            if (_native != IntPtr::Zero) dpc_close(_native.ToPointer());
            _native = IntPtr::Zero;
            _disposed = true;
        }

        property UInt32 Width { UInt32 get() { return _width; } }
        property UInt32 Height { UInt32 get() { return _height; } }
        property UInt32 MaxCells { UInt32 get() { return _maxCells; } }
        property String^ TextureName { String^ get() { return _textureName; } }
        property String^ FenceName { String^ get() { return _fenceName; } }
        property IntPtr SharedTextureHandle { IntPtr get() { return IntPtr(dpc_get_texture_handle(Handle())); } }
        property IntPtr SharedFenceHandle { IntPtr get() { return IntPtr(dpc_get_fence_handle(Handle())); } }
        property IntPtr DevicePointer { IntPtr get() { return IntPtr(dpc_get_device_ptr(Handle())); } }
        property IntPtr ResourcePointer { IntPtr get() { return IntPtr(dpc_get_resource_ptr(Handle())); } }
        property Int64 AdapterLuid { Int64 get() { return dpc_get_adapter_luid(Handle()); } }
        property UInt64 CompletedValue { UInt64 get() { return dpc_get_completed_value(Handle()); } }
        property UInt64 DroppedFrames { UInt64 get() { return dpc_get_dropped_frames(Handle()); } }

        void Show(String^ title, UInt32 width, UInt32 height) {
            if (String::IsNullOrWhiteSpace(title)) title = "DirectPort Console";
            IntPtr chars = Marshal::StringToHGlobalUni(title);
            const bool shown = dpc_show_window(Handle(), static_cast<const wchar_t*>(chars.ToPointer()), width, height);
            Marshal::FreeHGlobal(chars);
            if (!shown) throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
        }

        ConsoleWindowState Pump(UInt32 waitMilliseconds) {
            DPC_WINDOW_STATE native = {};
            dpc_pump_window(Handle(), waitMilliseconds, &native);
            ConsoleWindowState managed;
            managed.Alive = native.alive != 0;
            managed.Width = native.width;
            managed.Height = native.height;
            managed.MouseX = native.mouse_x;
            managed.MouseY = native.mouse_y;
            managed.LeftDown = native.left_down != 0;
            managed.WheelDelta = native.wheel_delta;
            managed.KeyCode = native.key_code;
            managed.ResizeSerial = native.resize_serial;
            return managed;
        }

        void EnableManifest(String^ manifestName) {
            if (String::IsNullOrWhiteSpace(manifestName))
                throw gcnew ArgumentException("Manifest name is required.", "manifestName");
            IntPtr chars = Marshal::StringToHGlobalUni(manifestName);
            const bool enabled = dpc_enable_manifest(Handle(), static_cast<const wchar_t*>(chars.ToPointer()));
            Marshal::FreeHGlobal(chars);
            if (!enabled) throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
        }

        bool TryPresent(array<UInt32>^ cells, UInt32 columns, UInt32 rows, Single timeSeconds, UInt64 publishValue) {
            if (cells == nullptr) throw gcnew ArgumentNullException("cells");
            if (columns == 0 || rows == 0 || static_cast<UInt64>(columns) * rows != static_cast<UInt64>(cells->LongLength))
                throw gcnew ArgumentException("cells.Length must equal columns * rows.", "cells");
            if (cells->LongLength > _maxCells)
                throw gcnew ArgumentException("Cell payload exceeds MaxCells.", "cells");
            pin_ptr<UInt32> packed = &cells[0];
            const bool submitted = dpc_try_present(Handle(), packed, cells->Length, columns, rows, timeSeconds, publishValue);
            if (!submitted && dpc_last_error()[0])
                throw gcnew InvalidOperationException(gcnew String(dpc_last_error()));
            return submitted;
        }
    };

    public value struct CanvasWindowState {
        bool Alive;
        Int32 Width;
        Int32 Height;
        Int32 MouseX;
        Int32 MouseY;
        bool LeftDown;
        bool RightDown;
        Int32 WheelDelta;
        Int32 KeyCode;
        Char CharCode;
        bool IsDoubleClick;
        UInt64 ResizeSerial;
    };

    public value struct TextMetrics {
        Single Width;
        Single Height;
    };

    // Immediate-mode 2D GPU Canvas powered by Direct2D / DirectWrite on D3D12.
    public ref class GpuCanvas2D sealed : IDisposable {
    private:
        IntPtr _native;
        bool _disposed;

        DP2D_HANDLE Handle() {
            if (_disposed || _native == IntPtr::Zero)
                throw gcnew ObjectDisposedException("GpuCanvas2D");
            return _native.ToPointer();
        }

    public:
        GpuCanvas2D(UInt32 width, UInt32 height, String^ title)
            : _native(IntPtr::Zero), _disposed(false) {
            if (width == 0 || height == 0)
                throw gcnew ArgumentOutOfRangeException("width", "Width and height must be non-zero.");

            IntPtr titleChars = Marshal::StringToHGlobalUni(title ? title : "DirectPort Desktop");
            DP2D_HANDLE native = dp2d_create(width, height, static_cast<const wchar_t*>(titleChars.ToPointer()));
            Marshal::FreeHGlobal(titleChars);

            if (!native)
                throw gcnew InvalidOperationException(gcnew String(dp2d_last_error()));
            _native = IntPtr(native);
        }

        ~GpuCanvas2D() {
            this->!GpuCanvas2D();
            GC::SuppressFinalize(this);
        }

        !GpuCanvas2D() {
            if (_disposed) return;
            if (_native != IntPtr::Zero) dp2d_close(_native.ToPointer());
            _native = IntPtr::Zero;
            _disposed = true;
        }

        CanvasWindowState Pump(UInt32 waitMilliseconds) {
            DP2D_WINDOW_STATE native = {};
            dp2d_pump_window(Handle(), waitMilliseconds, &native);
            CanvasWindowState managed;
            managed.Alive = native.alive != 0;
            managed.Width = native.width;
            managed.Height = native.height;
            managed.MouseX = native.mouse_x;
            managed.MouseY = native.mouse_y;
            managed.LeftDown = native.left_down != 0;
            managed.RightDown = native.right_down != 0;
            managed.WheelDelta = native.wheel_delta;
            managed.KeyCode = native.key_code;
            managed.CharCode = static_cast<Char>(native.char_code);
            managed.IsDoubleClick = native.is_double_click != 0;
            managed.ResizeSerial = native.resize_serial;
            return managed;
        }

        void SetCursor(CanvasCursor cursor) {
            dp2d_set_cursor(Handle(), static_cast<DP2D_CURSOR>(cursor));
        }

        bool BeginFrame(UInt32 clearColorArgb) {
            return dp2d_begin_frame(Handle(), clearColorArgb);
        }

        void FillRect(Single x, Single y, Single w, Single h, UInt32 argb, Single radius) {
            dp2d_fill_rect(Handle(), x, y, w, h, argb, radius);
        }

        void DrawRect(Single x, Single y, Single w, Single h, UInt32 argb, Single strokeWidth, Single radius) {
            dp2d_draw_rect(Handle(), x, y, w, h, argb, strokeWidth, radius);
        }

        void DrawLine(Single x1, Single y1, Single x2, Single y2, UInt32 argb, Single strokeWidth) {
            dp2d_draw_line(Handle(), x1, y1, x2, y2, argb, strokeWidth);
        }

        void DrawText(String^ text, Single x, Single y, Single maxWidth, Single maxHeight, UInt32 argb, Single fontSize, String^ fontFamily, bool bold, CanvasTextAlign alignment) {
            if (String::IsNullOrEmpty(text)) return;
            pin_ptr<const wchar_t> textChars = PtrToStringChars(text);
            pin_ptr<const wchar_t> fontChars = String::IsNullOrEmpty(fontFamily) ? nullptr : PtrToStringChars(fontFamily);
            dp2d_draw_text(Handle(), textChars, x, y, maxWidth, maxHeight, argb, fontSize, fontChars, bold, static_cast<int32_t>(alignment));
        }

        TextMetrics MeasureText(String^ text, Single fontSize, String^ fontFamily, bool bold) {
            TextMetrics m;
            m.Width = 0.0f;
            m.Height = 0.0f;
            if (String::IsNullOrEmpty(text)) return m;
            pin_ptr<const wchar_t> textChars = PtrToStringChars(text);
            pin_ptr<const wchar_t> fontChars = String::IsNullOrEmpty(fontFamily) ? nullptr : PtrToStringChars(fontFamily);
            float w = 0.0f, h = 0.0f;
            dp2d_measure_text(Handle(), textChars, fontSize, fontChars, bold, &w, &h);
            m.Width = w;
            m.Height = h;
            return m;
        }

        void PushClip(Single x, Single y, Single w, Single h) {
            dp2d_push_clip(Handle(), x, y, w, h);
        }

        void PopClip() {
            dp2d_pop_clip(Handle());
        }

        void PushTransform(Single offsetX, Single offsetY, Single scale) {
            dp2d_push_transform(Handle(), offsetX, offsetY, scale);
        }

        void PopTransform() {
            dp2d_pop_transform(Handle());
        }

        void SetOpacity(Single alpha) {
            dp2d_set_opacity(Handle(), alpha);
        }

        void DrawShadow(Single x, Single y, Single w, Single h, Single radius, Single blur, Single offsetY, UInt32 argb) {
            dp2d_draw_shadow(Handle(), x, y, w, h, radius, blur, offsetY, argb);
        }

        void DrawWireIcon(String^ iconName, Single x, Single y, Single size, UInt32 argb, Single strokeWidth) {
            if (String::IsNullOrEmpty(iconName)) return;
            pin_ptr<const wchar_t> iconChars = PtrToStringChars(iconName);
            dp2d_draw_wire_icon(Handle(), iconChars, x, y, size, argb, strokeWidth);
        }

        bool EndFrame() {
            return dp2d_end_frame(Handle());
        }

        bool SavePng(String^ filePath) {
            if (String::IsNullOrEmpty(filePath)) return false;
            pin_ptr<const wchar_t> pathChars = PtrToStringChars(filePath);
            return dp2d_save_png(Handle(), pathChars);
        }
    };

    // DirectPort Symmetric Dual-Duplex (Two-Lane) Endpoint
    // Lane 1: Ingress (Host -> Target / Control Stream)
    // Lane 2: Egress  (Target -> Host / Surface Stream)
    // Or vice-versa. Completely non-blocking and decoupled.
    public ref class GpuDuplexPort sealed : IDisposable {
    private:
        SharedResource^ _ingress;
        SharedResource^ _egress;
        String^ _name;
        bool _disposed;

    public:
        GpuDuplexPort(
            String^ portName,
            UInt32 width,
            UInt32 height,
            PixelFormat format,
            MemoryLayout layout)
            : _name(portName), _disposed(false) {
            
            if (String::IsNullOrWhiteSpace(portName))
                throw gcnew ArgumentException("Port name is required.", "portName");

            String^ inTex = portName + "_Ingress_Tex";
            String^ inFence = portName + "_Ingress_Fence";
            String^ egTex = portName + "_Egress_Tex";
            String^ egFence = portName + "_Egress_Fence";

            _ingress = gcnew SharedResource(width, height, format, layout, inTex, inFence);
            _egress = gcnew SharedResource(width, height, format, layout, egTex, egFence);
        }

        ~GpuDuplexPort() {
            this->!GpuDuplexPort();
            GC::SuppressFinalize(this);
        }

        !GpuDuplexPort() {
            if (_disposed) return;
            if (_ingress != nullptr) { delete _ingress; _ingress = nullptr; }
            if (_egress != nullptr) { delete _egress; _egress = nullptr; }
            _disposed = true;
        }

        property String^ Name { String^ get() { return _name; } }
        property SharedResource^ Ingress { SharedResource^ get() { return _ingress; } }
        property SharedResource^ Egress { SharedResource^ get() { return _egress; } }

        void SignalIngress(UInt64 serial) {
            _ingress->Signal(serial);
        }

        void SignalEgress(UInt64 serial) {
            _egress->Signal(serial);
        }

        bool TrySampleIngress(UInt64 minSerial, [Out] UInt64% completedSerial) {
            completedSerial = _ingress->CompletedValue;
            return completedSerial >= minSerial;
        }

        bool TrySampleEgress(UInt64 minSerial, [Out] UInt64% completedSerial) {
            completedSerial = _egress->CompletedValue;
            return completedSerial >= minSerial;
        }
    };
}


