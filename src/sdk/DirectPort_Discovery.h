// --- DirectPort_Discovery.h ---
// Lightweight, non-blocking UDP broadcast discovery protocol for DirectPort A/V streams.
// Eliminates kernel namespace polling thrash by broadcasting presence over local loopback / LAN.
// Defaults to 127.0.0.1 loopback for zero-prompt Windows Firewall IPC.

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

#define DIRECTPORT_DISCOVERY_PORT 3987
#define DIRECTPORT_MAGIC 0x44505254 // 'DPRT'

#pragma pack(push, 1)
struct DirectPortBeaconPacket {
    UINT32 magic;               // DIRECTPORT_MAGIC
    UINT32 version;             // 2 (version 2 adds audio)
    UINT32 width;
    UINT32 height;
    UINT32 format;              // DXGI_FORMAT
    UINT64 currentFrame;
    CHAR   streamName[64];
    CHAR   textureHandleName[128];
    CHAR   fenceHandleName[128];
    CHAR   hostName[64];
    // Audio integration
    UINT32 hasAudio;            // 1 if audio ring buffer present
    UINT32 sampleRate;          // e.g. 48000
    UINT32 channels;            // e.g. 2
    CHAR   audioBufferName[128];// Shared memory ring buffer name
};
#pragma pack(pop)

class DirectPortDiscoveryBroadcaster {
public:
    DirectPortDiscoveryBroadcaster() : m_socket(INVALID_SOCKET), m_initialized(false) {}
    ~DirectPortDiscoveryBroadcaster() { Stop(); }

    bool Start() {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) { WSACleanup(); return false; }

        BOOL broadcast = TRUE;
        setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

        // Non-blocking mode
        u_long mode = 1;
        ioctlsocket(m_socket, FIONBIO, &mode);

        memset(&m_destAddrLan, 0, sizeof(m_destAddrLan));
        m_destAddrLan.sin_family = AF_INET;
        m_destAddrLan.sin_port = htons(DIRECTPORT_DISCOVERY_PORT);
        m_destAddrLan.sin_addr.s_addr = INADDR_BROADCAST;

        memset(&m_destAddrLoopback, 0, sizeof(m_destAddrLoopback));
        m_destAddrLoopback.sin_family = AF_INET;
        m_destAddrLoopback.sin_port = htons(DIRECTPORT_DISCOVERY_PORT);
        m_destAddrLoopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        m_initialized = true;
        return true;
    }

    void Broadcast(const char* streamName, UINT width, UINT height, UINT format, 
                   const wchar_t* texHandleName, const wchar_t* fenceHandleName, UINT64 frame,
                   bool hasAudio = false, UINT32 sampleRate = 48000, UINT32 channels = 2, const wchar_t* audioBufferName = L"") {
        if (!m_initialized || m_socket == INVALID_SOCKET) return;

        DirectPortBeaconPacket pkt = {};
        pkt.magic = DIRECTPORT_MAGIC;
        pkt.version = 2;
        pkt.width = width;
        pkt.height = height;
        pkt.format = format;
        pkt.currentFrame = frame;

        strncpy_s(pkt.streamName, streamName, _TRUNCATE);
        WideCharToMultiByte(CP_UTF8, 0, texHandleName, -1, pkt.textureHandleName, sizeof(pkt.textureHandleName), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, fenceHandleName, -1, pkt.fenceHandleName, sizeof(pkt.fenceHandleName), NULL, NULL);
        gethostname(pkt.hostName, sizeof(pkt.hostName));

        pkt.hasAudio = hasAudio ? 1 : 0;
        pkt.sampleRate = sampleRate;
        pkt.channels = channels;
        if (hasAudio && audioBufferName) {
            WideCharToMultiByte(CP_UTF8, 0, audioBufferName, -1, pkt.audioBufferName, sizeof(pkt.audioBufferName), NULL, NULL);
        }

        // Send to loopback (guaranteed zero firewall popups for local consumers)
        sendto(m_socket, (const char*)&pkt, sizeof(pkt), 0, (sockaddr*)&m_destAddrLoopback, sizeof(m_destAddrLoopback));
        // Send to LAN broadcast
        sendto(m_socket, (const char*)&pkt, sizeof(pkt), 0, (sockaddr*)&m_destAddrLan, sizeof(m_destAddrLan));
    }

    void Stop() {
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            WSACleanup();
        }
        m_initialized = false;
    }

private:
    SOCKET m_socket;
    sockaddr_in m_destAddrLan;
    sockaddr_in m_destAddrLoopback;
    bool m_initialized;
};

class DirectPortDiscoveryListener {
public:
    struct DiscoveredStream {
        std::string streamName;
        std::wstring textureHandleName;
        std::wstring fenceHandleName;
        std::string hostName;
        UINT width;
        UINT height;
        UINT format;
        UINT64 lastFrame;
        bool hasAudio;
        UINT32 sampleRate;
        UINT32 channels;
        std::wstring audioBufferName;
        std::chrono::steady_clock::time_point lastSeen;
    };

    DirectPortDiscoveryListener() : m_socket(INVALID_SOCKET), m_initialized(false) {}
    ~DirectPortDiscoveryListener() { Stop(); }

    // Start with loopbackOnly = true (default) to avoid Windows Firewall exception prompts completely.
    // Set loopbackOnly = false for cross-network multi-machine discovery.
    bool Start(bool loopbackOnly = true) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

        m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) { WSACleanup(); return false; }

        BOOL reuse = TRUE;
        setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

        sockaddr_in bindAddr = {};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(DIRECTPORT_DISCOVERY_PORT);
        bindAddr.sin_addr.s_addr = loopbackOnly ? htonl(INADDR_LOOPBACK) : INADDR_ANY;

        if (bind(m_socket, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
            closesocket(m_socket);
            WSACleanup();
            return false;
        }

        u_long mode = 1; // Non-blocking
        ioctlsocket(m_socket, FIONBIO, &mode);

        m_initialized = true;
        return true;
    }

    // Non-blocking poll for discovery packets
    void Poll(std::vector<DiscoveredStream>& streams) {
        if (!m_initialized || m_socket == INVALID_SOCKET) return;

        DirectPortBeaconPacket pkt = {};
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);

        while (true) {
            int bytes = recvfrom(m_socket, (char*)&pkt, sizeof(pkt), 0, (sockaddr*)&fromAddr, &fromLen);
            if (bytes <= 0) break;

            if (bytes == sizeof(pkt) && pkt.magic == DIRECTPORT_MAGIC) {
                WCHAR wTex[128] = {}, wFence[128] = {}, wAudio[128] = {};
                MultiByteToWideChar(CP_UTF8, 0, pkt.textureHandleName, -1, wTex, 128);
                MultiByteToWideChar(CP_UTF8, 0, pkt.fenceHandleName, -1, wFence, 128);
                if (pkt.hasAudio) {
                    MultiByteToWideChar(CP_UTF8, 0, pkt.audioBufferName, -1, wAudio, 128);
                }

                bool found = false;
                auto now = std::chrono::steady_clock::now();
                for (auto& s : streams) {
                    if (s.streamName == pkt.streamName && s.hostName == pkt.hostName) {
                        s.width = pkt.width;
                        s.height = pkt.height;
                        s.format = pkt.format;
                        s.lastFrame = pkt.currentFrame;
                        s.hasAudio = (pkt.hasAudio != 0);
                        s.sampleRate = pkt.sampleRate;
                        s.channels = pkt.channels;
                        s.audioBufferName = wAudio;
                        s.lastSeen = now;
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    DiscoveredStream s = {};
                    s.streamName = pkt.streamName;
                    s.hostName = pkt.hostName;
                    s.textureHandleName = wTex;
                    s.fenceHandleName = wFence;
                    s.width = pkt.width;
                    s.height = pkt.height;
                    s.format = pkt.format;
                    s.lastFrame = pkt.currentFrame;
                    s.hasAudio = (pkt.hasAudio != 0);
                    s.sampleRate = pkt.sampleRate;
                    s.channels = pkt.channels;
                    s.audioBufferName = wAudio;
                    s.lastSeen = now;
                    streams.push_back(s);
                }
            }
        }

        // Clean up stale streams (older than 3 seconds)
        auto now = std::chrono::steady_clock::now();
        streams.erase(
            std::remove_if(streams.begin(), streams.end(),
                [&](const DiscoveredStream& s) {
                    return std::chrono::duration_cast<std::chrono::seconds>(now - s.lastSeen).count() > 3;
                }),
            streams.end()
        );
    }

    void Stop() {
        if (m_socket != INVALID_SOCKET) {
            closesocket(m_socket);
            m_socket = INVALID_SOCKET;
            WSACleanup();
        }
        m_initialized = false;
    }

private:
    SOCKET m_socket;
    bool m_initialized;
};
