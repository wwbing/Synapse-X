// ─── UdpReceiver.cpp ───────────────────────────────────────────
// Non-blocking UDP receive loop + out-of-order reassembly + LZ4 decompress.
//
// Hot path: TryReceive() drains all queued datagrams, reassembles
// chunks using ReassemblyBuffer, and decompresses complete frames.
// All buffers are pre-allocated — zero heap allocation per frame.
//
// UDP datagram layout (updated 20-byte PacketHeader):
//   ┌─────────────────────────┬────────────────────────────────┐
//   │     PacketHeader        │       payload (LZ4 slice)      │
//   │      20 bytes           │        ≤ MAX_PAYLOAD_SIZE      │
//   └─────────────────────────┴────────────────────────────────┘

#include "UdpReceiver.h"
#include "PacketHeader.h"

#include <lz4.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

UdpReceiver::~UdpReceiver() {
    Cleanup();
}

bool UdpReceiver::Initialize(uint16_t port) {
    if (m_initialized) Cleanup();

    // ── WinSock startup ──────────────────────────────────
    WSADATA wsaData = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "[UdpReceiver] WSAStartup FAILED: %d\n", WSAGetLastError());
        return false;
    }
    m_wsaStarted = true;

    // ── Create UDP socket ────────────────────────────────
    m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == INVALID_SOCKET) {
        fprintf(stderr, "[UdpReceiver] socket() FAILED: %d\n", WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── Set non-blocking mode ────────────────────────────
    // This is critical: we want to drain ALL queued packets
    // without ever blocking on recvfrom.
    u_long nonBlocking = 1;
    if (ioctlsocket(m_socket, FIONBIO, &nonBlocking) != 0) {
        fprintf(stderr, "[UdpReceiver] ioctlsocket(FIONBIO) FAILED: %d\n",
                WSAGetLastError());
        Cleanup();
        return false;
    }

    // ── Enlarge receive buffer ───────────────────────────
    int bufSize = 256 * 1024;  // 256 KB
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&bufSize), sizeof(bufSize));

    // ── Bind to local port ───────────────────────────────
    sockaddr_in localAddr = {};
    localAddr.sin_family      = AF_INET;
    localAddr.sin_port        = htons(port);
    localAddr.sin_addr.s_addr = INADDR_ANY;  // listen on all interfaces

    if (bind(m_socket, reinterpret_cast<const sockaddr*>(&localAddr),
             sizeof(localAddr)) == SOCKET_ERROR) {
        fprintf(stderr, "[UdpReceiver] bind(:%u) FAILED: %d\n",
                port, WSAGetLastError());
        Cleanup();
        return false;
    }

    // m_decompressBuf is NOT pre-sized here — it grows lazily
    // on the first complete frame based on PacketHeader width/height.

    m_initialized = true;
    fprintf(stderr, "[UdpReceiver] Ready -- listening on 0.0.0.0:%u, "
            "non-blocking, recv buffer %d KB, dynamic ROI\n",
            port, bufSize / 1024);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  TryReceive (hot path)
// ═══════════════════════════════════════════════════════════════

bool UdpReceiver::TryReceive(std::vector<uint8_t>& outFrame,
                             uint32_t& outFrameId) {
    if (!m_initialized) return false;

    bool anyFrameCompleted = false;

    // ── Drain ALL queued UDP datagrams ────────────────────
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
            if (err == WSAEWOULDBLOCK) {
                // No more data in socket buffer — exit drain loop.
                break;
            }
            // Real error (unlikely in steady state)
            fprintf(stderr, "[UdpReceiver] recvfrom ERROR: %d\n", err);
            break;
        }

        if (bytes < static_cast<int>(sizeof(PacketHeader))) {
            // Datagram too small — drop silently.
            continue;
        }

        m_totalPackets++;
        m_totalBytes += static_cast<uint64_t>(bytes);

        bool frameCompleted = ProcessDatagram(m_recvBuf, bytes);
        if (frameCompleted) {
            anyFrameCompleted = true;
        }
    }

    // ── Return the latest completed frame ─────────────────
    if (anyFrameCompleted) {
        // m_decompressBuf was already resized and filled by
        // DecompressCurrentFrame.  Copy to caller verbatim.
        outFrame = m_decompressBuf;
        outFrameId = m_lastDecodedFrameId;
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
//  ProcessDatagram — per-packet logic
// ═══════════════════════════════════════════════════════════════

bool UdpReceiver::ProcessDatagram(const uint8_t* data, int len) {
    // ── 1. Parse header ───────────────────────────────────
    const auto* header = reinterpret_cast<const PacketHeader*>(data);

    // ── 2. Validate magic ─────────────────────────────────
    if (header->magic != PROTOCOL_MAGIC) {
        // Not our protocol — drop silently.
        return false;
    }

    // ── 3. Extract payload ────────────────────────────────
    const uint8_t* payload = data + sizeof(PacketHeader);
    const uint16_t payloadSize = header->payloadSize;

    // Safety check: payload must fit within received datagram
    if (static_cast<int>(sizeof(PacketHeader) + payloadSize) > len) {
        fprintf(stderr, "[UdpReceiver] Truncated datagram: header says %u payload, "
                "got %d total bytes\n", payloadSize, len);
        return false;
    }

    // ── 4. Frame transition logic ─────────────────────────
    const uint32_t frameId      = header->frameId;
    const uint32_t totalSize    = header->totalSize;
    const uint16_t totalChunks  = header->totalChunks;
    const uint16_t chunkIndex   = header->chunkIndex;
    const uint16_t frameWidth   = header->width;
    const uint16_t frameHeight  = header->height;

    if (!m_buffer.HasActiveFrame()) {
        // First frame ever — start collecting.
        m_buffer.StartFrame(frameId, totalSize, totalChunks,
                            frameWidth, frameHeight);
        m_activeFrameIncomplete = true;
    } else if (IsNewerFrameId(frameId, m_buffer.expectedFrameId)) {
        // Newer frame arrived — flush old partial frame.
        // This is the iron law of low-latency vision:
        // never wait for stragglers from a stale frame.
        if (m_activeFrameIncomplete) {
            m_totalDropped++;
        }
        m_buffer.StartFrame(frameId, totalSize, totalChunks,
                            frameWidth, frameHeight);
        m_activeFrameIncomplete = true;
    } else if (frameId != m_buffer.expectedFrameId) {
        // Stale packet from an older frame — drop.
        return false;
    }
    // else: frameId == expectedFrameId — continue collecting.

    // ── 5. Insert chunk ───────────────────────────────────
    bool inserted = m_buffer.InsertChunk(chunkIndex, payload, payloadSize);
    if (!inserted) {
        // Duplicate or out-of-range — already handled.
        return false;
    }

    // ── 6. Check completion ───────────────────────────────
    if (m_buffer.IsComplete()) {
        m_activeFrameIncomplete = false;
        // Detect frame ID gaps for drop-rate accounting.
        if (m_hasDecodedAnyFrame) {
            // Count skipped frame IDs (host sent but we never started).
            // Use int32_t arithmetic to handle wrap correctly.
            int32_t gap = static_cast<int32_t>(frameId - m_lastDecodedFrameId) - 1;
            if (gap > 0) {
                m_totalDropped += static_cast<uint64_t>(gap);
            }
        }

        // Decompress into m_decompressBuf (reused every frame).
        // Width/height already stored in m_buffer from StartFrame.
        bool ok = DecompressCurrentFrame(m_decompressBuf);
        if (ok) {
            m_totalFramesReceived++;
            m_lastDecodedFrameId = frameId;
            m_hasDecodedAnyFrame = true;

            // Cache dimensions for caller to interpret outFrame
            m_lastFrameWidth  = m_buffer.frameWidth;
            m_lastFrameHeight = m_buffer.frameHeight;

            // Reset buffer for next frame.
            m_buffer.Reset();

            return true;  // signal: frame completed
        } else {
            // Decompression failed — count as dropped, reset, continue.
            m_totalDropped++;
            m_buffer.Reset();
        }
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════
//  DecompressCurrentFrame
// ═══════════════════════════════════════════════════════════════

bool UdpReceiver::DecompressCurrentFrame(std::vector<uint8_t>& outFrame) {
    const int compressedSize = static_cast<int>(m_buffer.totalSize);
    const uint32_t rawSize = m_buffer.GetRawFrameSize();

    // Ensure output buffer is large enough (reuses capacity if shrinking)
    outFrame.resize(rawSize);

    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(m_buffer.data.data()),
        reinterpret_cast<char*>(outFrame.data()),
        compressedSize,
        static_cast<int>(outFrame.size())
    );

    const int expectedSize = static_cast<int>(rawSize);
    if (result != expectedSize) {
        if (result < 0) {
            fprintf(stderr, "[UdpReceiver] LZ4_decompress_safe ERROR: %d "
                    "(compressed=%d bytes, raw=%ux%ux4=%u, frameId=%u)\n",
                    result, compressedSize,
                    m_buffer.frameWidth, m_buffer.frameHeight, rawSize,
                    m_buffer.expectedFrameId);
        } else {
            fprintf(stderr, "[UdpReceiver] LZ4 size mismatch: got %d, expected %d "
                    "(raw=%ux%ux4=%u, compressed=%d bytes, frameId=%u)\n",
                    result, expectedSize,
                    m_buffer.frameWidth, m_buffer.frameHeight, rawSize,
                    compressedSize, m_buffer.expectedFrameId);
        }
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Cleanup
// ═══════════════════════════════════════════════════════════════

void UdpReceiver::Cleanup() {
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_wsaStarted) {
        WSACleanup();
        m_wsaStarted = false;
    }
    m_initialized = false;
    m_buffer.Reset();
    m_lastFrameWidth  = 0;
    m_lastFrameHeight = 0;
}

} // namespace SynapseX
