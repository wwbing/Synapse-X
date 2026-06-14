// ─── UdpSender.cpp ───────────────────────────────────────────
// Hot path: SendCompressedFrame() — zero heap allocations.
// Each chunk is assembled on the stack and fired via sendto().
//
// Layout of a single UDP datagram:
//   ┌──────────────┬─────────────────────────────────┐
//   │ PacketHeader │        payload (LZ4 slice)      │
//   │   16 bytes    │         ≤ MAX_PAYLOAD_SIZE      │
//   └──────────────┴─────────────────────────────────┘

#include "UdpSender.h"
#include "PacketHeader.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ── Constants ──────────────────────────────────────────────
static constexpr int kSendBufSize = 256 * 1024;  // 256 KB socket buffer

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

UdpSender::~UdpSender() {
    Cleanup();
}

bool UdpSender::Initialize(const std::string& targetIp, uint16_t port) {
    if (m_initialized) Cleanup();

    // ── WinSock startup ──────────────────────────────────
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[UdpSender] WSAStartup FAILED: %d\n", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    // ── Create UDP socket ────────────────────────────────
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        fprintf(stderr, "[UdpSender] socket() FAILED: %d\n", WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── Enlarge send buffer ──────────────────────────────
    int bufSize = kSendBufSize;
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // ── Resolve target address ───────────────────────────
    std::memset(&m_targetAddr, 0, sizeof(m_targetAddr));
    m_targetAddr.sin_family = AF_INET;
    m_targetAddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, targetIp.c_str(), &m_targetAddr.sin_addr) != 1) {
        fprintf(stderr, "[UdpSender] inet_pton FAILED for '%s': %d\n",
                targetIp.c_str(), WSAGetLastError());
        Cleanup();
        return false;
    }

    m_initialized = true;
    fprintf(stderr, "[UdpSender] Ready -- target %s:%u, send buffer %d KB\n",
            targetIp.c_str(), port, kSendBufSize / 1024);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Send (hot path)
// ═══════════════════════════════════════════════════════════════

bool UdpSender::SendCompressedFrame(const uint8_t* compressedData,
                                     uint32_t totalSize,
                                     uint32_t frameId) {
    if (!m_initialized) return false;
    if (totalSize == 0)   return false;

    // ── Calculate chunk count ────────────────────────────
    const uint16_t totalChunks = static_cast<uint16_t>(
        (totalSize + MAX_PAYLOAD_SIZE - 1) / MAX_PAYLOAD_SIZE);

    // ── Stack-allocated packet buffer ────────────────────
    //    Zero heap allocation in the send loop.
    constexpr size_t kPacketBufSize = sizeof(PacketHeader) + MAX_PAYLOAD_SIZE;
    uint8_t packetBuf[kPacketBufSize];

    // Pre-fill the header fields that stay constant per frame.
    auto* header = reinterpret_cast<PacketHeader*>(packetBuf);
    header->magic       = PROTOCOL_MAGIC;
    header->frameId     = frameId;
    header->totalChunks = totalChunks;
    header->totalSize   = totalSize;

    const uint8_t* src = compressedData;
    uint32_t remaining = totalSize;

    for (uint16_t i = 0; i < totalChunks; ++i) {
        // Per-chunk header fields
        header->chunkIndex  = i;
        header->payloadSize = (remaining > MAX_PAYLOAD_SIZE)
                              ? MAX_PAYLOAD_SIZE
                              : static_cast<uint16_t>(remaining);

        // Copy payload into packet buffer (right after the header)
        std::memcpy(packetBuf + sizeof(PacketHeader),
                    src,
                    header->payloadSize);

        // Fire
        int totalPacketSize = static_cast<int>(sizeof(PacketHeader) + header->payloadSize);
        int sent = sendto(m_socket,
                          reinterpret_cast<const char*>(packetBuf),
                          totalPacketSize,
                          0,
                          reinterpret_cast<const sockaddr*>(&m_targetAddr),
                          sizeof(m_targetAddr));

        if (sent == SOCKET_ERROR) {
            int err = WSAGetLastError();
            fprintf(stderr, "[UdpSender] sendto FAILED chunk %u/%u: WSAGetLastError=%d\n",
                    i + 1, totalChunks, err);
            return false;
        }

        src       += header->payloadSize;
        remaining -= header->payloadSize;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Cleanup
// ═══════════════════════════════════════════════════════════════

void UdpSender::Cleanup() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
    m_initialized = false;
}

} // namespace SynapseX
