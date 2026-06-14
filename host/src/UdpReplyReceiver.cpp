// ─── UdpReplyReceiver.cpp ────────────────────────────────────
// Non-blocking UDP listener for Client inference replies.
// Parses ReplyHeader + DetectionRaw[] and maps model-space
// coordinates to screen coordinates using the Host's ROI offset.

#include "UdpReplyReceiver.h"
#include "ReplyPacket.h"

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

UdpReplyReceiver::~UdpReplyReceiver() {
    Cleanup();
}

bool UdpReplyReceiver::Initialize(uint16_t port) {
    if (m_initialized) Cleanup();

    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[ReplyRecv] WSAStartup FAILED: %d\n", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        fprintf(stderr, "[ReplyRecv] socket() FAILED: %d\n", WSAGetLastError());
        Cleanup();
        return false;
    }

    // Non-blocking — we drain in the main loop, never block.
    u_long nonBlocking = 1;
    ioctlsocket(m_socket, FIONBIO, &nonBlocking);

    int bufSize = 64 * 1024;  // 64 KB — generous for small replies
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_port        = htons(port);
    localAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr),
             sizeof(localAddr)) == SOCKET_ERROR) {
        fprintf(stderr, "[ReplyRecv] bind(:%u) FAILED: %d\n",
                port, WSAGetLastError());
        Cleanup();
        return false;
    }

    m_initialized = true;
    fprintf(stderr, "[ReplyRecv] Ready -- listening on 0.0.0.0:%u\n", port);
    return true;
}

void UdpReplyReceiver::Cleanup() {
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

// ═══════════════════════════════════════════════════════════════
//  ROI setup
// ═══════════════════════════════════════════════════════════════

void UdpReplyReceiver::SetRoiParams(int roiW, int roiH,
                                     int screenW, int screenH) {
    m_roiW    = roiW;
    m_roiH    = roiH;
    m_screenW = screenW;
    m_screenH = screenH;
    RecalculateRoiOffset();
}

void UdpReplyReceiver::RecalculateRoiOffset() {
    m_roiX = (m_screenW - m_roiW) / 2;
    m_roiY = (m_screenH - m_roiH) / 2;
}

// ═══════════════════════════════════════════════════════════════
//  Receive loop (hot path)
// ═══════════════════════════════════════════════════════════════

bool UdpReplyReceiver::ReceiveReplies(std::vector<Detection>& outDetections,
                                       uint32_t& outLatestFrameId) {
    if (!m_initialized) return false;

    bool anyReceived = false;

    while (true) {
        sockaddr_in fromAddr = {};
        int fromLen = sizeof(fromAddr);
        int bytes = recvfrom(m_socket,
                             reinterpret_cast<char*>(m_recvBuf),
                             kRecvBufSize,
                             0,
                             reinterpret_cast<sockaddr*>(&fromAddr),
                             &fromLen);

        if (bytes == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) break;  // no more data
            fprintf(stderr, "[ReplyRecv] recvfrom ERROR: %d\n", err);
            break;
        }

        // ── Parse header ──────────────────────────────────
        if (bytes < static_cast<int>(sizeof(ReplyHeader))) continue;

        const auto* header = reinterpret_cast<const ReplyHeader*>(m_recvBuf);
        if (header->magic != REPLY_MAGIC) continue;

        uint16_t numDets = header->numDets;
        if (numDets > MAX_DETS_PER_REPLY) {
            fprintf(stderr, "[ReplyRecv] numDets=%u exceeds max %u — clamping\n",
                    numDets, MAX_DETS_PER_REPLY);
            numDets = MAX_DETS_PER_REPLY;
        }

        int expectedLen = static_cast<int>(
            sizeof(ReplyHeader) + numDets * sizeof(DetectionRaw));
        if (bytes < expectedLen) {
            fprintf(stderr, "[ReplyRecv] Truncated reply: got %d, expected %d\n",
                    bytes, expectedLen);
            continue;
        }

        // ── Parse detections and map to screen coordinates ──
        const auto* rawDets = reinterpret_cast<const DetectionRaw*>(
            m_recvBuf + sizeof(ReplyHeader));

        for (uint16_t i = 0; i < numDets; ++i) {
            Detection det;
            det.x1         = static_cast<float>(m_roiX) + rawDets[i].x1;
            det.y1         = static_cast<float>(m_roiY) + rawDets[i].y1;
            det.x2         = static_cast<float>(m_roiX) + rawDets[i].x2;
            det.y2         = static_cast<float>(m_roiY) + rawDets[i].y2;
            det.confidence = rawDets[i].confidence;
            det.classId    = rawDets[i].classId;
            outDetections.push_back(det);
        }

        outLatestFrameId = header->frameId;
        anyReceived = true;
    }

    return anyReceived;
}

} // namespace SynapseX
