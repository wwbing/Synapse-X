#pragma once

// ── Client → Host reply protocol ────────────────────────────
// After inference, the Client sends detection results back to
// the Host over UDP. Each reply fits in a single datagram.
//
// Layout:
//   ┌──────────────┬─────────────────────────────────┐
//   │ ReplyHeader  │     DetectionRaw[]              │
//   │   16 bytes   │   numDets × 24 bytes             │
//   └──────────────┴─────────────────────────────────┘

#include <cstdint>

namespace SynapseX {

constexpr uint16_t REPLY_MAGIC = 0x5359;  // 'SY' — distinct from data magic (0x5358)
constexpr uint16_t MAX_DETS_PER_REPLY = 50;  // 50 × 24 + 16 = 1216 bytes, fits in MTU

#pragma pack(push, 1)
struct ReplyHeader {
    uint16_t magic   = REPLY_MAGIC;
    uint32_t frameId = 0;          // matches Host's frameId
    uint16_t numDets = 0;          // number of DetectionRaw entries
    uint8_t  padding[8] = {};      // reserved
};
#pragma pack(pop)
static_assert(sizeof(ReplyHeader) == 16, "ReplyHeader must be 16 bytes");

#pragma pack(push, 1)
struct DetectionRaw {
    float    x1, y1;               // top-left, model pixel coords
    float    x2, y2;               // bottom-right
    float    confidence;
    uint32_t classId;              // 0 = enemy, 1 = teammate
};
#pragma pack(pop)
static_assert(sizeof(DetectionRaw) == 24, "DetectionRaw must be 24 bytes");

} // namespace SynapseX
