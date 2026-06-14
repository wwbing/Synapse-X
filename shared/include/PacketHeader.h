#pragma once
#include <cstdint>

// ── Application-layer fragmentation protocol ────────────────
// Each compressed frame is split into N UDP-friendly chunks.
// The receiver reassembles chunks sharing the same FrameID.
//
//         ┌──────────────┬──────────────────────────────────┐
//         │ PacketHeader │          Payload (LZ4)            │
//         │   16 bytes    │         ≤ MAX_PAYLOAD            │
//         └──────────────┴──────────────────────────────────┘

namespace SynapseX {

// Payload cap — keeps total UDP datagram well under 1472-byte
// Ethernet MTU, leaving headroom for IP/UDP headers and VPN overhead.
constexpr uint16_t MAX_PAYLOAD_SIZE = 1400;

// Magic number for basic integrity check on the wire.
constexpr uint16_t PROTOCOL_MAGIC = 0x5358;  // 'SX'

#pragma pack(push, 1)
struct PacketHeader {
    uint16_t magic        = PROTOCOL_MAGIC;  // protocol eye-catcher
    uint32_t frameId      = 0;               // monotonic frame counter
    uint16_t totalChunks  = 0;               // pieces in this frame
    uint16_t chunkIndex   = 0;               // 0-based piece index
    uint32_t totalSize    = 0;               // uncompressed compressed-data size
    uint16_t payloadSize  = 0;               // bytes in this packet's payload
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be 16 bytes");

// Maximum payload bytes per packet — matching the header cap.
constexpr uint16_t MAX_CHUNKS_PER_FRAME = 65535;

} // namespace SynapseX
