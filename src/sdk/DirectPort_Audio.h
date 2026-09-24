// --- DirectPort_Audio.h ---
// DirectPort Interprocess Audio Protocol
// Provides lock-free circular ring buffer over Win32 Shared Memory (NT Object Manager)
// with WASAPI Loopback (speaker) and Endpoint (mic) capture/playback support.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <sddl.h>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <wrl/client.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

#define DP_AUDIO_MAGIC 0x44504155 // 'DPAU'
#define DP_AUDIO_CAPACITY_SAMPLES (48000 * 2 * 4) // 4 seconds of stereo 48kHz float

using Microsoft::WRL::ComPtr;

#pragma pack(push, 1)
struct DirectPortAudioSharedHeader {
    UINT32 magic;               // DP_AUDIO_MAGIC
    UINT32 sampleRate;          // 48000
    UINT32 channels;            // 2 (Stereo)
    UINT32 bitsPerSample;       // 32 (IEEE Float)
    UINT32 capacitySamples;     // DP_AUDIO_CAPACITY_SAMPLES
    std::atomic<UINT64> writeHead; // Monotonically increasing sample counter
};
#pragma pack(pop)

// --- Shared Memory Audio Ring Buffer Producer ---
class DirectPortAudioRingProducer {
public:
    DirectPortAudioRingProducer() : m_hMap(nullptr), m_pHeader(nullptr), m_pSamples(nullptr) {}
    ~DirectPortAudioRingProducer() { Close(); }

    bool Initialize(const wchar_t* bufferName, UINT32 sampleRate = 48000, UINT32 channels = 2) {
        Close();

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = FALSE;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;AU)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

        DWORD totalSize = sizeof(DirectPortAudioSharedHeader) + (DP_AUDIO_CAPACITY_SAMPLES * sizeof(float));
        m_hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, totalSize, bufferName);
        if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);

        if (!m_hMap) return false;

        void* view = MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, totalSize);
        if (!view) { CloseHandle(m_hMap); m_hMap = nullptr; return false; }

        m_pHeader = reinterpret_cast<DirectPortAudioSharedHeader*>(view);
        m_pHeader->magic = DP_AUDIO_MAGIC;
        m_pHeader->sampleRate = sampleRate;
        m_pHeader->channels = channels;
        m_pHeader->bitsPerSample = 32;
        m_pHeader->capacitySamples = DP_AUDIO_CAPACITY_SAMPLES;
        m_pHeader->writeHead.store(0, std::memory_order_relaxed);

        m_pSamples = reinterpret_cast<float*>(reinterpret_cast<BYTE*>(view) + sizeof(DirectPortAudioSharedHeader));
        return true;
    }

    void WriteSamples(const float* pInterleaved, UINT32 sampleCount) {
        if (!m_pHeader || !m_pSamples) return;

        UINT64 head = m_pHeader->writeHead.load(std::memory_order_relaxed);
        UINT32 cap = m_pHeader->capacitySamples;

        for (UINT32 i = 0; i < sampleCount; ++i) {
            m_pSamples[(head + i) % cap] = pInterleaved[i];
        }

        m_pHeader->writeHead.store(head + sampleCount, std::memory_order_release);
    }

    void Close() {
        if (m_pHeader) {
            UnmapViewOfFile(m_pHeader);
            m_pHeader = nullptr;
            m_pSamples = nullptr;
        }
        if (m_hMap) {
            CloseHandle(m_hMap);
            m_hMap = nullptr;
        }
    }

private:
    HANDLE m_hMap;
    DirectPortAudioSharedHeader* m_pHeader;
    float* m_pSamples;
};

// --- Shared Memory Audio Ring Buffer Consumer ---
class DirectPortAudioRingConsumer {
public:
    DirectPortAudioRingConsumer() : m_hMap(nullptr), m_pHeader(nullptr), m_pSamples(nullptr), m_readHead(0) {}
    ~DirectPortAudioRingConsumer() { Close(); }

    bool Open(const wchar_t* bufferName) {
        Close();

        m_hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, bufferName);
        if (!m_hMap) return false;

        void* view = MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, 0);
        if (!view) { CloseHandle(m_hMap); m_hMap = nullptr; return false; }

        m_pHeader = reinterpret_cast<DirectPortAudioSharedHeader*>(view);
        if (m_pHeader->magic != DP_AUDIO_MAGIC) {
            Close();
            return false;
        }

        m_pSamples = reinterpret_cast<float*>(reinterpret_cast<BYTE*>(view) + sizeof(DirectPortAudioSharedHeader));
        m_readHead = m_pHeader->writeHead.load(std::memory_order_acquire);
        return true;
    }

    // Reads up to maxSamples. Returns actual samples read.
    UINT32 ReadSamples(float* pOutBuffer, UINT32 maxSamples) {
        if (!m_pHeader || !m_pSamples) return 0;

        UINT64 writeHead = m_pHeader->writeHead.load(std::memory_order_acquire);
        if (writeHead <= m_readHead) return 0;

        UINT64 available = writeHead - m_readHead;
        UINT32 cap = m_pHeader->capacitySamples;

        // If consumer fell behind by more than buffer capacity, jump forward
        if (available > cap) {
            m_readHead = writeHead - (cap / 2);
            available = cap / 2;
        }

        UINT32 toRead = (UINT32)std::min((UINT64)maxSamples, available);
        for (UINT32 i = 0; i < toRead; ++i) {
            pOutBuffer[i] = m_pSamples[(m_readHead + i) % cap];
        }

        m_readHead += toRead;
        return toRead;
    }

    void Close() {
        if (m_pHeader) {
            UnmapViewOfFile(m_pHeader);
            m_pHeader = nullptr;
            m_pSamples = nullptr;
        }
        if (m_hMap) {
            CloseHandle(m_hMap);
            m_hMap = nullptr;
        }
        m_readHead = 0;
    }

    bool IsConnected() const { return m_pHeader != nullptr; }

private:
    HANDLE m_hMap;
    DirectPortAudioSharedHeader* m_pHeader;
    float* m_pSamples;
    UINT64 m_readHead;
};

// --- WASAPI Loopback Capture Service ---
class DirectPortWASAPICapture {
public:
    DirectPortWASAPICapture() : m_hThread(nullptr), m_hStopEvent(nullptr), m_running(false) {}
    ~DirectPortWASAPICapture() { Stop(); }

    bool Start(DirectPortAudioRingProducer* pRing) {
        Stop();
        m_pRing = pRing;
        m_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_running = true;

        m_hThread = CreateThread(nullptr, 0, CaptureThreadProc, this, 0, nullptr);
        return m_hThread != nullptr;
    }

    void Stop() {
        if (m_running) {
            m_running = false;
            if (m_hStopEvent) SetEvent(m_hStopEvent);
            if (m_hThread) {
                WaitForSingleObject(m_hThread, 1000);
                CloseHandle(m_hThread);
                m_hThread = nullptr;
            }
            if (m_hStopEvent) {
                CloseHandle(m_hStopEvent);
                m_hStopEvent = nullptr;
            }
        }
    }

private:
    static DWORD WINAPI CaptureThreadProc(LPVOID pParam) {
        auto* self = reinterpret_cast<DirectPortWASAPICapture*>(pParam);
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
            CoUninitialize();
            return 1;
        }

        ComPtr<IMMDevice> defaultDevice;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &defaultDevice))) {
            CoUninitialize();
            return 1;
        }

        ComPtr<IAudioClient> audioClient;
        if (FAILED(defaultDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient))) {
            CoUninitialize();
            return 1;
        }

        WAVEFORMATEX* pwfx = nullptr;
        if (FAILED(audioClient->GetMixFormat(&pwfx))) {
            CoUninitialize();
            return 1;
        }

        REFERENCE_TIME hnsBufferDuration = 10000000; // 1 second
        if (FAILED(audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, hnsBufferDuration, 0, pwfx, nullptr))) {
            CoTaskMemFree(pwfx);
            CoUninitialize();
            return 1;
        }

        ComPtr<IAudioCaptureClient> captureClient;
        if (FAILED(audioClient->GetService(IID_PPV_ARGS(&captureClient)))) {
            CoTaskMemFree(pwfx);
            CoUninitialize();
            return 1;
        }

        audioClient->Start();

        std::vector<float> floatConversionBuffer;

        while (WaitForSingleObject(self->m_hStopEvent, 10) == WAIT_TIMEOUT) {
            UINT32 packetLength = 0;
            if (FAILED(captureClient->GetNextPacketSize(&packetLength))) break;

            while (packetLength > 0) {
                BYTE* pData = nullptr;
                UINT32 numFramesAvailable = 0;
                DWORD flags = 0;

                if (SUCCEEDED(captureClient->GetBuffer(&pData, &numFramesAvailable, &flags, nullptr, nullptr))) {
                    if (numFramesAvailable > 0 && self->m_pRing) {
                        UINT32 channels = pwfx->nChannels;
                        UINT32 totalSamples = numFramesAvailable * channels;

                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            floatConversionBuffer.assign(totalSamples, 0.0f);
                            self->m_pRing->WriteSamples(floatConversionBuffer.data(), totalSamples);
                        } else if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                                   (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && 
                                    reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                            // Already float
                            self->m_pRing->WriteSamples(reinterpret_cast<const float*>(pData), totalSamples);
                        } else if (pwfx->wBitsPerSample == 16) {
                            // Convert 16-bit PCM to float
                            floatConversionBuffer.resize(totalSamples);
                            const int16_t* pShort = reinterpret_cast<const int16_t*>(pData);
                            for (UINT32 s = 0; s < totalSamples; ++s) {
                                floatConversionBuffer[s] = pShort[s] / 32768.0f;
                            }
                            self->m_pRing->WriteSamples(floatConversionBuffer.data(), totalSamples);
                        }
                    }
                    captureClient->ReleaseBuffer(numFramesAvailable);
                }
                captureClient->GetNextPacketSize(&packetLength);
            }
        }

        audioClient->Stop();
        CoTaskMemFree(pwfx);
        CoUninitialize();
        return 0;
    }

    DirectPortAudioRingProducer* m_pRing = nullptr;
    HANDLE m_hThread;
    HANDLE m_hStopEvent;
    bool m_running;
};
