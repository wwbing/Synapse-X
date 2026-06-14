// ─── UdpReplySender.cpp ────────────────────────────────────────
// Packs Detection results into a compact UDP reply and sends to Host.

#include "UdpReplySender.h"
#include "TrtInference.h"
#include "ReplyPacket.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ── Constants ──────────────────────────────────────────────
static constexpr int kSendBufSize = 64 * 1024;  // 64 KB socket send buffer

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

UdpReplySender::~UdpReplySender() {
    Cleanup();
}

bool UdpReplySender::Initialize(const std::string& hostIp, uint16_t port) {
    if (m_initialized) Cleanup();

    // ── WinSock startup ──────────────────────────────────
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[UdpReplySender] WSAStartup FAILED: %d\n",
                WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    // ── Create UDP socket ────────────────────────────────
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        fprintf(stderr, "[UdpReplySender] socket() FAILED: %d\n",
                WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── Enlarge send buffer ──────────────────────────────
    int bufSize = kSendBufSize;
    setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // ── Set non-blocking (fire-and-forget) ───────────────
    u_long nonBlocking = 1;
    ioctlsocket(m_socket, FIONBIO, &nonBlocking);

    // ── Resolve target address ───────────────────────────
    std::memset(&m_targetAddr, 0, sizeof(m_targetAddr));
    m_targetAddr.sin_family = AF_INET;
    m_targetAddr.sin_port   = htons(port);

    if (inet_pton(AF_INET, hostIp.c_str(), &m_targetAddr.sin_addr) != 1) {
        fprintf(stderr, "[UdpReplySender] inet_pton FAILED for '%s': %d\n",
                hostIp.c_str(), WSAGetLastError());
        Cleanup();
        return false;
    }

    m_initialized = true;
    fprintf(stderr, "[UdpReplySender] Ready — sending to %s:%u\n",
            hostIp.c_str(), port);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SendReplies (hot path)
// ═══════════════════════════════════════════════════════════════

bool UdpReplySender::SendReplies(uint32_t frameId,
                                  const std::vector<Detection>& dets) {
    if (!m_initialized) return false;

    // Cap detections to max per reply
    uint16_t numDets = static_cast<uint16_t>(
        std::min(dets.size(), static_cast<size_t>(MAX_DETS_PER_REPLY)));

    // ── Build packet on stack ────────────────────────────
    constexpr size_t kMaxPacket = sizeof(ReplyHeader)
                                + MAX_DETS_PER_REPLY * sizeof(DetectionRaw);
    alignas(64) uint8_t packetBuf[kMaxPacket];

    auto* header = reinterpret_cast<ReplyHeader*>(packetBuf);
    header->magic   = REPLY_MAGIC;
    header->frameId = frameId;
    header->numDets = numDets;
    // padding already zeroed by ReplyHeader default init

    auto* rawDets = reinterpret_cast<DetectionRaw*>(packetBuf + sizeof(ReplyHeader));
    for (uint16_t i = 0; i < numDets; ++i) {
        rawDets[i].x1         = dets[i].x1;
        rawDets[i].y1         = dets[i].y1;
        rawDets[i].x2         = dets[i].x2;
        rawDets[i].y2         = dets[i].y2;
        rawDets[i].confidence = dets[i].confidence;
        rawDets[i].classId    = static_cast<uint32_t>(dets[i].classId);
    }

    int totalBytes = static_cast<int>(
        sizeof(ReplyHeader) + numDets * sizeof(DetectionRaw));

    int sent = sendto(m_socket,
                      reinterpret_cast<const char*>(packetBuf),
                      totalBytes,
                      0,
                      reinterpret_cast<const sockaddr*>(&m_targetAddr),
                      sizeof(m_targetAddr));

    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            fprintf(stderr, "[UdpReplySender] sendto FAILED: %d\n", err);
        }
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Cleanup
// ═══════════════════════════════════════════════════════════════

void UdpReplySender::Cleanup() {
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
