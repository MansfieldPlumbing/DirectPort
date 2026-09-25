// --- DirectPort_Camera.h ---
// High-performance Win32 Media Foundation Camera Capture for DirectPort.
// The Source Reader runs asynchronously: each finished read stores the frame
// and requests the next one, so the render loop never blocks on the camera.
// The render loop copies the newest frame (if any) with CopyLatestFrame().

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <string>
#include <vector>
#include <atomic>
#include <algorithm>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

struct CameraDeviceInfo {
    int index = 0;
    std::wstring friendlyName;
    std::wstring symbolicLink;
};

class DirectPortCameraCapture {
public:
    DirectPortCameraCapture() = default;
    DirectPortCameraCapture(const DirectPortCameraCapture&) = delete;
    DirectPortCameraCapture& operator=(const DirectPortCameraCapture&) = delete;
    ~DirectPortCameraCapture() { Shutdown(); }

    static std::vector<CameraDeviceInfo> EnumerateCameras() {
        std::vector<CameraDeviceInfo> result;
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL)))
            return result;

        ComPtr<IMFAttributes> pAttributes;
        if (SUCCEEDED(MFCreateAttributes(&pAttributes, 1))) {
            pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            UINT32 count = 0;
            IMFActivate** ppDevices = nullptr;
            if (SUCCEEDED(MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &count)) && count > 0) {
                for (UINT32 i = 0; i < count; ++i) {
                    CameraDeviceInfo info = {};
                    info.index = (int)i;
                    WCHAR* pName = nullptr;
                    UINT32 cchName = 0;
                    if (SUCCEEDED(ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &pName, &cchName)) && pName) {
                        info.friendlyName = pName;
                        CoTaskMemFree(pName);
                    } else {
                        info.friendlyName = L"Camera " + std::to_wstring(i);
                    }
                    WCHAR* pLink = nullptr;
                    UINT32 cchLink = 0;
                    if (SUCCEEDED(ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &pLink, &cchLink)) && pLink) {
                        info.symbolicLink = pLink;
                        CoTaskMemFree(pLink);
                    }
                    result.push_back(info);
                    ppDevices[i]->Release();
                }
                CoTaskMemFree(ppDevices);
            }
        }
        MFShutdown();
        return result;
    }

    bool Initialize(int deviceIndex = 0) {
        Shutdown();
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) return false;
        m_initializedMF = true;

        ComPtr<IMFAttributes> pAttributes;
        if (FAILED(MFCreateAttributes(&pAttributes, 1))) return false;
        if (FAILED(pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) return false;

        UINT32 count = 0;
        IMFActivate** ppDevices = nullptr;
        if (FAILED(MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &count)) || count == 0) return false;
        if (deviceIndex < 0 || (UINT32)deviceIndex >= count) deviceIndex = 0;
        m_deviceIndex = deviceIndex;

        WCHAR* pName = nullptr;
        UINT32 cchName = 0;
        if (SUCCEEDED(ppDevices[deviceIndex]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &pName, &cchName)) && pName) {
            m_deviceName = pName;
            CoTaskMemFree(pName);
        } else {
            m_deviceName = L"Camera " + std::to_wstring(deviceIndex);
        }

        ComPtr<IMFMediaSource> pSource;
        HRESULT hr = ppDevices[deviceIndex]->ActivateObject(IID_PPV_ARGS(&pSource));
        for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
        CoTaskMemFree(ppDevices);
        if (FAILED(hr)) return false;

        m_callback = Microsoft::WRL::Make<ReaderCallback>(this);
        if (!m_callback) return false;

        ComPtr<IMFAttributes> pReaderAttributes;
        if (FAILED(MFCreateAttributes(&pReaderAttributes, 3))) return false;
        pReaderAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        pReaderAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        pReaderAttributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, m_callback.Get());
        if (FAILED(MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttributes.Get(), &m_sourceReader))) return false;

        if (!ChooseFormat()) return false;

        m_frame.assign((size_t)m_width * m_height * 4, 0);
        m_isCapturing = true;
        if (FAILED(m_sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr))) {
            Shutdown();
            return false;
        }
        return true;
    }

    // Copies the newest frame into `pDest` (rows of `destPitch` bytes) if one
    // arrived since `lastSerial`.  Never blocks on the camera.
    bool CopyLatestFrame(BYTE* pDest, UINT destPitch, UINT64& lastSerial) {
        if (!pDest || !m_isCapturing) return false;
        AcquireSRWLockShared(&m_frameLock);
        const bool fresh = m_frameSerial != lastSerial;
        if (fresh) {
            const size_t rowBytes = (size_t)m_width * 4;
            for (UINT y = 0; y < m_height; ++y)
                memcpy(pDest + (size_t)destPitch * y, m_frame.data() + rowBytes * y, rowBytes);
            lastSerial = m_frameSerial;
        }
        ReleaseSRWLockShared(&m_frameLock);
        return fresh;
    }

    void Shutdown() {
        if (m_isCapturing.exchange(false) && m_sourceReader)
            m_sourceReader->Flush(MF_SOURCE_READER_ALL_STREAMS);
        if (m_callback) {
            m_callback->Detach();   // waits for a callback in progress
            m_callback.Reset();
        }
        m_sourceReader.Reset();     // breaks the reader <-> callback cycle
        if (m_initializedMF) {
            MFShutdown();
            m_initializedMF = false;
        }
    }

    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }
    bool IsActive() const { return m_isCapturing; }
    int GetDeviceIndex() const { return m_deviceIndex; }
    const std::wstring& GetDeviceName() const { return m_deviceName; }

private:
    class ReaderCallback : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceReaderCallback> {
    public:
        explicit ReaderCallback(DirectPortCameraCapture* owner) : m_owner(owner) {}
        void Detach() {
            AcquireSRWLockExclusive(&m_lock);
            m_owner = nullptr;
            ReleaseSRWLockExclusive(&m_lock);
        }
        STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override {
            AcquireSRWLockShared(&m_lock);
            if (m_owner) m_owner->OnSample(hr, flags, sample);
            ReleaseSRWLockShared(&m_lock);
            return S_OK;
        }
        STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
        STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
    private:
        SRWLOCK m_lock = SRWLOCK_INIT;
        DirectPortCameraCapture* m_owner;
    };

    bool ChooseFormat() {
        ComPtr<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(&outputType))) return false;
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

        bool found = SUCCEEDED(m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get()));
        for (DWORD i = 0; !found; ++i) {
            ComPtr<IMFMediaType> nativeType;
            if (FAILED(m_sourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType))) break;
            found = SUCCEEDED(m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nativeType.Get())) &&
                    SUCCEEDED(m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get()));
        }
        if (!found) return false;

        ComPtr<IMFMediaType> currentType;
        if (FAILED(m_sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType))) return false;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (!w || !h) return false;
        m_width = w;
        m_height = h;
        return true;
    }

    // Runs on a Media Foundation work-queue thread.
    void OnSample(HRESULT hr, DWORD flags, IMFSample* sample) {
        if (!m_isCapturing) return;
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            m_isCapturing = false;   // unplugged or taken by an exclusive app
            return;
        }
        if (sample) StoreFrame(sample);
        if (FAILED(m_sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr)))
            m_isCapturing = false;
    }

    void StoreFrame(IMFSample* sample) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) return;
        const size_t rowBytes = (size_t)m_width * 4;

        // Honour the buffer's real stride (it may be padded or bottom-up).
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* raw = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
        } else if (SUCCEEDED(buffer->Lock(&raw, nullptr, &length)) && length >= rowBytes * m_height) {
            scan0 = raw;
            pitch = (LONG)rowBytes;
        } else {
            if (raw) buffer->Unlock();
            return;
        }

        AcquireSRWLockExclusive(&m_frameLock);
        for (UINT y = 0; y < m_height; ++y)
            memcpy(m_frame.data() + rowBytes * y, scan0 + (ptrdiff_t)pitch * y, rowBytes);
        ++m_frameSerial;
        ReleaseSRWLockExclusive(&m_frameLock);

        if (raw) buffer->Unlock();
        else buffer2d->Unlock2D();
    }

    ComPtr<IMFSourceReader> m_sourceReader;
    Microsoft::WRL::ComPtr<ReaderCallback> m_callback;
    SRWLOCK m_frameLock = SRWLOCK_INIT;
    std::vector<BYTE> m_frame;          // tightly packed BGRA, width * 4 per row
    UINT64 m_frameSerial = 0;
    UINT m_width = 0;
    UINT m_height = 0;
    int m_deviceIndex = 0;
    std::wstring m_deviceName;
    std::atomic<bool> m_isCapturing{ false };
    bool m_initializedMF = false;
};
