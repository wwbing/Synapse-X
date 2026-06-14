#pragma once

// ─── UdpReplySender ──────────────────────────────────────────
// Sends inference detections back to Host over UDP.
//
// Lifecycle:
//   1. Initialize(hostIp, port) — create UDP socket, set target.
//   2. SendReplies(frameId, dets) — pack and fire single datagram.
//   3. Cleanup() — close socket.
//
// The reply datagram is: ReplyHeader (16B) + DetectionRaw[] (N×24B).
// All fields are little-endian (native x64).

#include <cstdint>
#include <string>
#include <vector>
#include <winsock2.h>

namespace SynapseX {

struct Detection;  // forward from TrtInference.h

class UdpReplySender {
public:
    UdpReplySender() = default;
    ~UdpReplySender();

    UdpReplySender(const UdpReplySender&) = delete;
    UdpReplySender& operator=(const UdpReplySender&) = delete;
    UdpReplySender(UdpReplySender&&) = delete;
    UdpReplySender& operator=(UdpReplySender&&) = delete;

    // Connect to Host's reply port (usually 8889).
    bool Initialize(const std::string& hostIp, uint16_t port = 8889);

    // Send detection results for a single frame to Host.
    // dets: inference output (max 50, silently truncated if exceeded).
    bool SendReplies(uint32_t frameId, const std::vector<Detection>& dets);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

private:
    SOCKET      m_socket      = INVALID_SOCKET;
    sockaddr_in m_targetAddr  = {};
    bool        m_initialized = false;
    bool        m_wsaStarted  = false;
};

} // namespace SynapseX
