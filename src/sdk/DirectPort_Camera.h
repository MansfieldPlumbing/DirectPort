// --- DirectPort_Camera.h ---
// High-performance Win32 Media Foundation Camera Capture for DirectPort.
// Reads frames from active UVC webcam into contiguous BGRA/RGBA buffers with zero CPU thrash.

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
#include <string>
#include <vector>
#include <atomic>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

using Microsoft::WRL::ComPtr;

class DirectPortCameraCapture {
public:
    DirectPortCameraCapture() : m_width(0), m_height(0), m_isCapturing(false), m_initializedMF(false) {}
    ~DirectPortCameraCapture() { Shutdown(); }

    bool Initialize(int deviceIndex = 0) {
        Shutdown();

        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) return false;
        m_initializedMF = true;

        ComPtr<IMFAttributes> pAttributes;
        if (FAILED(MFCreateAttributes(&pAttributes, 1))) return false;
        if (FAILED(pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) return false;

        UINT32 count = 0;
        IMFActivate** ppDevices = nullptr;
        if (FAILED(MFEnumDeviceSources(pAttributes.Get(), &ppDevices, &count)) || count == 0) {
            return false;
        }

        if (deviceIndex < 0 || (UINT32)deviceIndex >= count) deviceIndex = 0;

        ComPtr<IMFMediaSource> pSource;
        hr = ppDevices[deviceIndex]->ActivateObject(IID_PPV_ARGS(&pSource));
        for (UINT32 i = 0; i < count; ++i) ppDevices[i]->Release();
        CoTaskMemFree(ppDevices);

        if (FAILED(hr)) return false;

        ComPtr<IMFAttributes> pReaderAttributes;
        if (FAILED(MFCreateAttributes(&pReaderAttributes, 2))) return false;
        pReaderAttributes->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);
        pReaderAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        if (FAILED(MFCreateSourceReaderFromMediaSource(pSource.Get(), pReaderAttributes.Get(), &m_sourceReader))) {
            return false;
        }

        ComPtr<IMFMediaType> outputType;
        if (FAILED(MFCreateMediaType(&outputType))) return false;
        outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);

        bool formatFound = false;
        for (DWORD i = 0; ; ++i) {
            ComPtr<IMFMediaType> nativeType;
            if (m_sourceReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &nativeType) == MF_E_NO_MORE_TYPES) {
                break;
            }
            if (SUCCEEDED(m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, nativeType.Get()))) {
                if (SUCCEEDED(m_sourceReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType.Get()))) {
                    formatFound = true;
                    break;
                }
            }
        }

        if (!formatFound) return false;

        ComPtr<IMFMediaType> currentType;
        if (FAILED(m_sourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &currentType))) return false;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        m_width = w;
        m_height = h;

        m_frameBuffer.resize(m_width * m_height * 4);
        m_isCapturing = true;
        return true;
    }

    // Returns true if a new frame was successfully read into outBuffer
    bool ReadFrame(std::vector<BYTE>& outBuffer) {
        if (!m_isCapturing || !m_sourceReader) return false;

        ComPtr<IMFSample> pSample;
        DWORD streamFlags = 0;
        LONGLONG timestamp = 0;
        HRESULT hr = m_sourceReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &streamFlags, &timestamp, &pSample);
        if (FAILED(hr) || !pSample) return false;

        ComPtr<IMFMediaBuffer> pBuffer;
        if (FAILED(pSample->ConvertToContiguousBuffer(&pBuffer))) return false;

        BYTE* pData = nullptr;
        DWORD currentLength = 0;
        if (FAILED(pBuffer->Lock(&pData, nullptr, &currentLength))) return false;

        outBuffer.resize(currentLength);
        memcpy(outBuffer.data(), pData, currentLength);
        pBuffer->Unlock();

        return true;
    }

    void Shutdown() {
        if (m_isCapturing) {
            m_isCapturing = false;
            if (m_sourceReader) {
                m_sourceReader->Flush(MF_SOURCE_READER_ALL_STREAMS);
                m_sourceReader.Reset();
            }
        }
        if (m_initializedMF) {
            MFShutdown();
            m_initializedMF = false;
        }
    }

    UINT GetWidth() const { return m_width; }
    UINT GetHeight() const { return m_height; }
    bool IsActive() const { return m_isCapturing; }

private:
    ComPtr<IMFSourceReader> m_sourceReader;
    UINT m_width;
    UINT m_height;
    std::vector<BYTE> m_frameBuffer;
    std::atomic<bool> m_isCapturing;
    bool m_initializedMF;
};
