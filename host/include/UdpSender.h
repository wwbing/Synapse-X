#pragma once

// ── UDP fragmentation & send module ───────────────────────────
// Splits a compressed LZ4 buffer into ≤1400-byte chunks,
// wraps each in a PacketHeader, and fires them over UDP.
//
// Design constraints:
//   · Zero heap allocation in the Send hot path (stack buffer only).
//   · sendto() calls are the only syscall; header assembly is pure memcpy.
//   · Socket send buffer is auto-sized for throughput.

#include <cstdint>
#include <string>
#include <winsock2.h>

namespace SynapseX {

class UdpSender {
public:
    UdpSender() = default;
    ~UdpSender();

    // Non-copyable, non-movable (owns socket)
    UdpSender(const UdpSender&) = delete;
    UdpSender& operator=(const UdpSender&) = delete;
    UdpSender(UdpSender&&) = delete;
    UdpSender& operator=(UdpSender&&) = delete;

    // Initialize WinSock, create UDP socket, set target address.
    // targetIp: "127.0.0.1" for local loopback, or remote IP for deployment.
    // port:     UDP port the Client listens on (default 8888).
    bool Initialize(const std::string& targetIp, uint16_t port = 8888);

    // Fragment `compressedData` (totalSize bytes) and send over UDP.
    // frameId: monotonically increasing frame counter, embedded in every chunk.
    // Returns true if ALL chunks were sent successfully.
    bool SendCompressedFrame(const uint8_t* compressedData,
                             uint32_t totalSize,
                             uint32_t frameId);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

private:
    SOCKET      m_socket      = ~0ull;  // INVALID_SOCKET
    sockaddr_in m_targetAddr  = {};
    bool        m_initialized = false;
    bool        m_wsaStarted  = false;
};

} // namespace SynapseX
