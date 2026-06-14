#pragma once

// ── UDP reply receiver ────────────────────────────────────────
// Listens on a UDP port (default 8889) for Client inference replies.
// Each reply carries detection bounding boxes in model pixel space;
// this module maps them to screen coordinates using the Host's
// known ROI offset for overlay rendering.
//
// Usage:
//   UdpReplyReceiver receiver;
//   receiver.Initialize(8889);
//   receiver.SetRoiParams(roiW, roiH, screenW, screenH);
//   // In main loop:
//   receiver.ReceiveReplies(detections, latestFrameId);

#include <cstdint>
#include <vector>
#include <winsock2.h>

namespace SynapseX {

// ── Screen-mapped detection (ready for overlay) ──────────────
struct Detection {
    float x1, y1, x2, y2;     // screen-space coordinates
    float confidence;
    uint32_t classId;          // 0 = enemy, 1 = teammate
};

class UdpReplyReceiver {
public:
    UdpReplyReceiver() = default;
    ~UdpReplyReceiver();

    UdpReplyReceiver(const UdpReplyReceiver&) = delete;
    UdpReplyReceiver& operator=(const UdpReplyReceiver&) = delete;

    // Bind UDP socket to `port` (default 8889).
    bool Initialize(uint16_t port = 8889);
    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

    // Set ROI and screen geometry for coordinate mapping.
    // Must be called before ReceiveReplies.
    void SetRoiParams(int roiW, int roiH, int screenW, int screenH);

    // Drain all queued reply datagrams from the socket buffer.
    // Returns detections mapped to screen coordinates.
    // outDetections: appended with decoded detections (not cleared first).
    // outLatestFrameId: set to the frameId of the most recent reply.
    // Returns true if at least one valid reply was received.
    bool ReceiveReplies(std::vector<Detection>& outDetections,
                        uint32_t& outLatestFrameId);

private:
    SOCKET m_socket      = INVALID_SOCKET;
    bool   m_initialized = false;
    bool   m_wsaStarted  = false;

    int m_roiW = 640, m_roiH = 640;
    int m_screenW = 1920, m_screenH = 1080;
    int m_roiX = 0, m_roiY = 0;  // computed center-crop offsets

    // Max possible reply: ReplyHeader(16) + 50*DetectionRaw(24) = 1216 bytes
    static constexpr int kRecvBufSize = 2048;
    alignas(64) uint8_t m_recvBuf[kRecvBufSize];

    void RecalculateRoiOffset();
};

} // namespace SynapseX
