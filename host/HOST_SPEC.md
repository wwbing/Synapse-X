# Synapse-X Host Specification

> **Target audience**: Client (Inference machine) developer.
> This document describes every detail the Client needs to receive, reassemble,
> decompress, and interpret data sent by the Host.

---

## 1. System Overview

```
┌── HOST (this machine) ──────────────────────────────────────────┐
│                                                                   │
│  GPU Desktop (e.g. 3840×2160)                                     │
│       │                                                           │
│       ▼ DXGI Desktop Duplication + GPU-side center-crop           │
│  BGRA pixel buffer  (640 × 640 × 4 = 1,638,400 bytes)            │
│       │                                                           │
│       ▼ LZ4 raw-block compress (LZ4_compress_fast, acceleration=1)│
│  Compressed payload  (typically 30–150 KB, varies by content)     │
│       │                                                           │
│       ▼ Fragment into chunks ≤ 1400 bytes each                    │
│  UDP packets  (each = 16-byte PacketHeader + ≤1400-byte payload)  │
│       │                                                           │
│       ▼ Winsock2 UDP send                                         │
│                                                                   │
└───────┬───────────────────────────────────────────────────────────┘
        │  Ethernet (physically isolated, direct or switch)
        ▼
┌── CLIENT (your machine) ──────────────────────────────────────────┐
│                                                                   │
│   UDP recv → reassemble by FrameID → LZ4 decompress → BGRA 640²   │
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

**Key numbers:**

| Parameter | Value |
|-----------|-------|
| ROI resolution | 640 × 640 (configurable, default) |
| Pixel format | BGRA, 8 bits per channel, 4 bytes per pixel |
| Raw frame size | 1,638,400 bytes (1.56 MiB) |
| Host target frame rate | 170 Hz (~5.88 ms per frame) |
| Compression algorithm | LZ4 raw block (NOT LZ4 frame format) |
| Compression level | `LZ4_compress_fast` acceleration=1 |

---

## 2. Wire Protocol: PacketHeader

**File:** `shared/PacketHeader.h`

Every UDP datagram carries a 16-byte header followed by payload:

```
Byte offset  0        2        6        8       10       14       16
          ┌────────┬────────┬────────┬────────┬────────┬────────┬──────┐
          │ magic  │frameId │totalCh │chunkIdx│totalSz │payldSz │ LZ4  │
          │  2B    │  4B    │  2B    │  2B    │  4B    │  2B    │ data │
          └────────┴────────┴────────┴────────┴────────┴────────┴──────┘
           ◄────────────── 16-byte PacketHeader ──────────► ◄─ payload ─►
           ◄────────── ≤ 1416 bytes total per UDP datagram ───────────►
```

### Field definitions

| Field | Type | Offset | Description |
|-------|------|--------|-------------|
| `magic` | `uint16_t` | 0 | Protocol eye-catcher. Always `0x5358` (ASCII 'SX'). Drop the packet if this does not match. |
| `frameId` | `uint32_t` | 2 | Monotonic frame counter. Starts at 0, wraps at 2³²⁻¹. The Host increments this for every captured frame. **All chunks of the same frame share the same frameId.** |
| `totalChunks` | `uint16_t` | 6 | Number of UDP packets this compressed frame is split into. |
| `chunkIndex` | `uint16_t` | 8 | Zero-based index of this packet within the frame. Range: `[0, totalChunks-1]`. |
| `totalSize` | `uint32_t` | 10 | Total size of the **compressed** LZ4 payload in bytes. This is the buffer you need to reassemble before calling `LZ4_decompress_safe`. |
| `payloadSize` | `uint16_t` | 14 | Number of valid LZ4 payload bytes in **this** packet. All chunks except possibly the last one are exactly `MAX_PAYLOAD_SIZE` bytes. The last chunk may be smaller. |

### Constants

```cpp
constexpr uint16_t MAX_PAYLOAD_SIZE = 1400;   // max payload per packet
constexpr uint16_t PROTOCOL_MAGIC   = 0x5358; // 'SX'
// Effective max UDP datagram = 16 (header) + 1400 (payload) = 1416 bytes
// This stays well under the 1472-byte Ethernet MTU payload limit.
```

### Byte order

All integer fields are **little-endian** (native x86_64 byte order; both Host and Client are assumed to be x64 Windows).

### Layout guarantee

```cpp
static_assert(sizeof(PacketHeader) == 16, "PacketHeader must be 16 bytes");
```

The struct is `#pragma pack(push, 1)` — no alignment padding between fields.

---

## 3. Reassembly Algorithm (Client side)

### Per-frame state

```cpp
struct ReassemblyBuffer {
    uint32_t expectedFrameId;       // frameId currently being collected
    uint32_t totalSize;             // total compressed size (from header)
    uint16_t totalChunks;           // expected number of chunks
    std::vector<bool> receivedMask; // which chunks have arrived
    std::vector<uint8_t> data;      // reassembled compressed buffer (totalSize bytes)
    int chunksReceived = 0;
};
```

### Algorithm (pseudocode)

```
1. Receive UDP datagram
2. If datagram size < 16: drop (too small for header)
3. Cast first 16 bytes to PacketHeader*
4. If header.magic != 0x5358: drop (not our protocol)
5. Extract payload: &datagram[16], length = header.payloadSize

6. If header.frameId > current reassemblyBuffer.expectedFrameId:
       // New frame arrived — flush old partial buffer, start fresh
       reassemblyBuffer.reset()
       reassemblyBuffer.expectedFrameId = header.frameId
       reassemblyBuffer.totalSize       = header.totalSize
       reassemblyBuffer.totalChunks     = header.totalChunks
       reassemblyBuffer.data.resize(header.totalSize)
       reassemblyBuffer.receivedMask.resize(header.totalChunks, false)

7. If header.frameId != reassemblyBuffer.expectedFrameId:
       drop (stale packet from old frame)

8. If reassemblyBuffer.receivedMask[header.chunkIndex] == false:
       // Copy payload into correct position
       offset = header.chunkIndex * MAX_PAYLOAD_SIZE
       memcpy(&reassemblyBuffer.data[offset], payload, header.payloadSize)
       reassemblyBuffer.receivedMask[header.chunkIndex] = true
       reassemblyBuffer.chunksReceived++

9. If reassemblyBuffer.chunksReceived == reassemblyBuffer.totalChunks:
       // Frame complete — decompress and use
       decompress(reassemblyBuffer.data) → BGRA 640×640
       reset reassemblyBuffer for next frame
```

### Edge cases

- **Out-of-order delivery**: The `chunkIndex` field lets you place each chunk at the correct byte offset regardless of arrival order.
- **Duplicate packets**: Check `receivedMask[chunkIndex]` before copying — skip if already received.
- **Stale packets**: If `frameId` is behind `expectedFrameId`, drop. If it's ahead, flush the old buffer and start the new frame.
- **Incomplete frames**: The Host sends every frame at ~170 Hz. If a frame is missing chunks after a timeout (e.g., 2× frame period = ~12 ms), discard it and wait for the next `frameId`.

---

## 4. Decompression

### Algorithm

The compressed payload is a **raw LZ4 block** (not an LZ4 frame with magic number / frame descriptor).

```cpp
#include <lz4.h>   // LZ4_decompress_safe

int decompressedSize = LZ4_decompress_safe(
    reinterpret_cast<const char*>(compressedData.data()),
    reinterpret_cast<char*>(outputBuffer.data()),
    static_cast<int>(compressedData.size()),   // compressed size
    static_cast<int>(outputBuffer.size())      // max output = 640*640*4
);

// decompressedSize should equal 1638400
```

### Verification

- Expected output size: **1,638,400 bytes** (640 × 640 × 4)
- If `decompressedSize != 1638400`: decompression error, discard frame
- `LZ4_decompress_safe` returns a negative value on corruption — handle this gracefully

### Compression parameters the Host uses

| Parameter | Value |
|-----------|-------|
| Function | `LZ4_compress_fast(src, dst, srcSize, dstCap, 1)` |
| Acceleration | 1 (fastest) |
| Bound | `LZ4_compressBound(1638400)` = 1,644,841 bytes (worst-case) |

You do NOT need to match these exactly on the Client side — `LZ4_decompress_safe` is compatible with any LZ4-compressed block regardless of acceleration setting.

---

## 5. Pixel Format

### Memory layout

```
Byte:    0      1      2      3      4      5      6      7     ...
       ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬─────
       │  B0  │  G0  │  R0  │  A0  │  B1  │  G1  │  R1  │  A1  │ ...
       └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┴─────
         ◄────────── pixel 0 ──────────► ◄────────── pixel 1 ──────►
```

- **Channel order**: B, G, R, A (Blue first, Alpha last)
- **Format name**: `DXGI_FORMAT_B8G8R8A8_UNORM` / `BGRA` in most graphics APIs
- **Alpha**: Always 255 (fully opaque) — the Host captures a desktop, not an alpha-blended surface
- **Orientation**: **Top-down** — the first row of pixels is the top of the screen. No vertical flip needed.
- **Row stride**: Exactly `width × 4` = 2,560 bytes per row. No extra padding.

### Converting for inference

Most inference models expect RGB or BGR input with normalized float values. Example conversion for a model expecting `[1, 3, 640, 640]` float32 tensor in RGB order:

```python
# numpy
bgra = np.frombuffer(raw_bytes, dtype=np.uint8).reshape(640, 640, 4)
rgb  = bgra[..., 2::-1]                    # drop alpha, reverse channel order
rgb_float = rgb.astype(np.float32) / 255.0
tensor = np.transpose(rgb_float, (2, 0, 1))  # HWC → CHW
tensor = np.expand_dims(tensor, axis=0)       # add batch dim
```

### Test reference

The Host test program (`SynapseX_Host_TestBmp.exe`) saves `test_roi.bmp` for visual verification. You can use this to calibrate your Client-side decoding. The BMP is 32-bit BGRA with `biHeight = -640` (top-down DIB).

---

## 6. Performance Profile

Measured on the Host machine (RTX 5070 Ti, 3840×2160 desktop, 640×640 ROI, RelWithDebInfo build):

| Stage | Typical time | Notes |
|-------|-------------|-------|
| GPU copy + Map | ~0.04–0.08 ms | GPU-side CopySubresourceRegion + staging Map |
| LZ4 compress | ~0.53–0.56 ms | LZ4_compress_fast, acceleration=1 |
| UDP fragment + send | ~0.15–0.35 ms | Stack buffer, sendto() per chunk |
| **Pipeline total** | **~0.75–1.10 ms** | Well within 5.88 ms budget for 170 Hz |

**Observed frame rates:**

| Scenario | FPS |
|----------|-----|
| Desktop idle (mouse moves only) | 2–5 |
| Light activity (scrolling, typing) | 25–77 |
| Heavy activity (window drag, video) | 220–370 |

> FPS is determined by how often Windows repaints the desktop, not by the pipeline.
> The pipeline itself can sustain 300+ FPS with no bottleneck.

**Compression ratio** is highly content-dependent:

| Desktop content | Typical compressed size | Ratio |
|-----------------|------------------------|-------|
| Solid color / simple UI | 30–50 KB | ~2–3% |
| Mixed text & images | 80–150 KB | ~5–9% |
| Full-screen video/game | 200–400 KB | ~12–25% |

---

## 7. Quick-start Test (for Client developer)

To test your Client implementation without the Host running, use these reference values:

### Test vector: all-gray desktop

```
Raw BGRA:   1,638,400 bytes of [31, 31, 31, 255] repeated
LZ4 output: First 4 bytes contain the LZ4 compressed block
            (variable size, typically ~100–500 bytes for uniform content)
```

### Capturing a reference frame

On the Host machine:
```powershell
cd host\build_x64\RelWithDebInfo
.\SynapseX_Host_TestBmp.exe
# Saves test_roi.bmp (visual check) and prints compression stats
```

---

## 8. Repository Structure

```
Synapse-X/                          # mono-repo root
├── .gitignore
├── CMakeLists.txt                  # root IDE context only
│
├── shared/                         # shared between Host and Client
│   └── include/
│       └── PacketHeader.h          # wire protocol (16-byte header)
│
├── host/                           # Host sender (this machine)
│   ├── CMakeLists.txt              # standalone CMake project
│   ├── CMakePresets.json           # VS 2026 x64 preset
│   ├── HOST_SPEC.md                # this file
│   ├── include/
│   │   ├── DxgiCapturer.h          # GPU screen capture
│   │   ├── Lz4Compressor.h         # LZ4 compression
│   │   └── UdpSender.h             # UDP fragmentation + send
│   ├── src/
│   │   ├── main.cpp                # production pipeline
│   │   ├── DxgiCapturer.cpp
│   │   ├── Lz4Compressor.cpp
│   │   └── UdpSender.cpp
│   └── test/
│       └── test_bmp.cpp            # capture + compress test
│
├── client/                         # Client receiver (other machine)
│   └── CMakeLists.txt              # TODO: UDP recv + LZ4 decompress + TensorRT
│
└── thirdparty/
    └── lz4-1.10.0/                 # LZ4 source (compiled directly)
```

### Include paths for Client

The shared protocol header is at:

```
shared/include/PacketHeader.h
```

In your Client CMakeLists.txt:

```cmake
target_include_directories(YourClientTarget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/include
)
```

In your C++ code:

```cpp
#include "PacketHeader.h"
```

### Building the Host (for reference)

```powershell
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# Binary: build_x64/RelWithDebInfo/SynapseX_Host.exe
```

Run: `.\SynapseX_Host.exe [target_ip] [port]` (defaults: 127.0.0.1:8888)

---

## 9. Future: Host → Client response channel

The current protocol is unidirectional (Host → Client). A future iteration will add:
- A small UDP response packet from Client → Host (inference result: bounding box coordinates)
- A `requestId` field matching Host frames to Client responses

This is not yet implemented — coordinate your Client design with this in mind.
