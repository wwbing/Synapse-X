#pragma once
#include <cstdint>

// ── Application-layer fragmentation protocol ────────────────
// Each compressed frame is split into N UDP-friendly chunks.
// The receiver reassembles chunks sharing the same FrameID.
//
//         ┌─────────────────────────┬────────────────────────┐
//         │     PacketHeader        │   Payload (LZ4 data)   │
//         │      24 bytes           │     ≤ MAX_PAYLOAD      │
//         └─────────────────────────┴────────────────────────┘

namespace SynapseX {

<<<<<<< HEAD
// Payload cap — keeps total UDP datagram well under 1472-byte
// Ethernet MTU: 24 (header) + 1400 (payload) = 1424 bytes.
=======
>>>>>>> b643835e2e70caa502511f387c4c4dce659956a0
constexpr uint16_t MAX_PAYLOAD_SIZE = 1400;
constexpr uint16_t PROTOCOL_MAGIC   = 0x5358;  // 'SX'

#pragma pack(push, 1)
struct PacketHeader {
<<<<<<< HEAD
    uint16_t magic        = PROTOCOL_MAGIC;  // protocol eye-catcher
    uint32_t frameId      = 0;               // monotonic frame counter
    uint16_t totalChunks  = 0;               // pieces in this frame
    uint16_t chunkIndex   = 0;               // 0-based piece index
    uint32_t totalSize    = 0;               // compressed data size (bytes)
    uint16_t payloadSize  = 0;               // bytes in this packet's payload
    uint16_t width        = 0;               // ROI width  (e.g., 640, 416)
    uint16_t height       = 0;               // ROI height (e.g., 640, 416)
    uint8_t  modelId      = 0;               // model selector (0=416, 1=640, ...)
    uint8_t  padding[3]   = {};              // keep 4-byte alignment
=======
    uint16_t magic        = PROTOCOL_MAGIC;  // 0: protocol eye-catcher
    uint32_t frameId      = 0;               // 2: monotonic frame counter
    uint16_t totalChunks  = 0;               // 6: pieces in this frame
    uint16_t chunkIndex   = 0;               // 8: 0-based piece index
    uint32_t totalSize    = 0;               // 10: compressed data size (bytes)
    uint16_t payloadSize  = 0;               // 14: bytes in this packet's payload
    uint16_t width        = 0;               // 16: ROI width
    uint16_t height       = 0;               // 18: ROI height
    uint8_t  modelId      = 0;               // 20: target model (maps to engine file)
    uint8_t  padding[3]   = {0};             // 21-23: reserved
>>>>>>> b643835e2e70caa502511f387c4c4dce659956a0
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 24, "PacketHeader must be 24 bytes");

constexpr uint16_t MAX_CHUNKS_PER_FRAME = 65535;

} // namespace SynapseX
<<<<<<< HEAD
=======

// ── Model switch signal ────────────────────────────────────
// Written by UdpReceiver when a valid new frame arrives.
// Read by TrtInference at the start of each Infer() call.
#include <atomic>
extern std::atomic<uint8_t> g_targetModelId;
>>>>>>> b643835e2e70caa502511f387c4c4dce659956a0
