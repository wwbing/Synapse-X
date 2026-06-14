# Synapse-X Client Specification

> **Target audience**: Host developer.
> Full pipeline doc: UDP recv → reassembly → LZ4 decompress → TRT inference → UDP reply.
> Use alongside `host/HOST_SPEC.md` and `shared/ReplyPacket.h`.

---

## 1. System Overview

```
┌── HOST ────────────────────────────────────────────────────────────┐
│  DXGI ROI capture → LZ4 compress → fragment → UDP send (:8888)     │
│  Also: UDP recv (:8889) ← detection results from Client            │
└───────┬──────────────────────────────┬─────────────────────────────┘
        │  Ethernet (direct cable)      │
        ▼                              ▲
┌── CLIENT ───────────────────────────────────────────────────────────┐
│  Stage 1: UDP recv (:8888), non-blocking drain                     │
│  Stage 2: ReassemblyBuffer (out-of-order, offset = idx × 1400)     │
│  Stage 3: LZ4_decompress_safe → verify W×H×4                       │
│  Stage 4: TRT inference (FP16, 416×416, ~3.5 ms)                   │
│  Stage 5: UDP reply → Host:8889 (ReplyHeader + DetectionRaw[])     │
└─────────────────────────────────────────────────────────────────────┘
```

### Key Numbers

| Category | Parameter | Value |
|----------|-----------|-------|
| **Network** | Data port | UDP 8888 |
| | Reply port | UDP 8889 |
| | Header size | 20 bytes |
| | Datagram max | 1420 bytes |
| | Socket mode | Non-blocking |
| **Decompress** | ROI | Dynamic, W×H from PacketHeader |
| | Raw bytes | W × H × 4 |
| | Verification | `LZ4_decompress_safe` result == W×H×4 |
| **Inference** | Model | 1×3×416×416 FP32 CHW → 300×6 FP32 |
| | Engine | `bf416.engine` (7.4 MB, FP16) |
| | GPU compute | ~1.57 ms |
| | Full infer | ~3.5 ms (preprocess + GPU + post) |
| **Reply** | Magic | `0x5359` ('SY') |
| | Header | 16 bytes |
| | Per-detection | 24 bytes (f32×5 + u32) |
| | Max packet | 1216 bytes (50 dets) |
| **Pipeline** | Full latency | ~3.5 ms (recv + infer + reply) |
| | Sustained FPS | 170 (under 5.88 ms budget) |

---

## 2. File Map

```
client/
├── CLIENT_SPEC.md               ← this file
├── CMakeLists.txt               ← LZ4 + CUDA 13.1 + TRT 10.16 + ws2_32
├── CMakePresets.json            ← VS 2026 x64 preset
├── model/
│   ├── bf416.onnx               ← portable model (9.3 MB)
│   └── bf416.engine             ← compiled engine (7.4 MB, GPU-bound)
├── include/
│   ├── ReassemblyBuffer.h       ← reassembly engine (struct, ~140 lines)
│   ├── UdpReceiver.h            ← UDP recv + LZ4 decompress
│   ├── TrtInference.h           ← TRT inference wrapper
│   └── UdpReplySender.h         ← reply channel
└── src/
    ├── UdpReceiver.cpp          ← recvfrom drain + reassemble + decompress
    ├── TrtInference.cpp         ← engine load + pre/infer/post
    ├── UdpReplySender.cpp       ← pack reply + sendto(Host:8889)
    └── main.cpp                 ← full pipeline + stats + BMP
shared/
├── include/
│   ├── PacketHeader.h           ← Host→Client protocol (20 bytes)
│   └── ReplyPacket.h            ← Client→Host protocol (16 + N×24 bytes)
```

### Dependency Graph

```
main.cpp
  ├── UdpReceiver.h ─── ReassemblyBuffer.h ─── PacketHeader.h
  ├── TrtInference.h ─── (CUDA + TensorRT SDK)
  └── UdpReplySender.h ─── ReplyPacket.h ─── TrtInference.h (Detection)
```

---

## 3. Build & Run

```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# Run (all defaults):
.\build_x64\RelWithDebInfo\SynapseX_Client.exe

# Explicit:
.\SynapseX_Client.exe [port=8888] [engine=../../model/bf416.engine] [hostIp=192.168.100.1]
```

**DLL PATH requirement:**
```powershell
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11...\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"
```

---

## 4. ReassemblyBuffer (Stage 1–2)

See `HOST_SPEC.md` §3 for the algorithm. Client implements it exactly:

- **Chunk offset**: `chunkIndex × 1400` — direct `memcpy`, zero heap
- **Frame switch**: `frameId > expected` → discard old frame immediately
- **Duplicate**: `receivedMask[chunkIndex]` bitmask, O(1) check
- **Dynamic ROI**: `frameWidth`/`frameHeight` from PacketHeader → carried to decompress
- **Pre-allocation**: 67 MB worst-case (4096²), steady-state zero allocation

---

## 5. UdpReceiver (Stage 3)

- `recvfrom()` in non-blocking loop until `WSAEWOULDBLOCK`
- Validates magic `0x5358`, checks truncation, extracts 20-byte header
- Calls `LZ4_decompress_safe` → verifies output == W×H×4
- `m_decompressBuf` resized lazily, reused across frames
- Records `m_lastFrameWidth`/`m_lastFrameHeight` for caller

### API
```cpp
bool TryReceive(vector<uint8_t>& outFrame, uint32_t& outFrameId);
uint16_t GetLastFrameWidth() const;
uint16_t GetLastFrameHeight() const;
uint64_t GetTotalFrames/Dropped/Packets/Bytes() const;
```

---

## 6. TrtInference (Stage 4)

### Preprocess: BGRA → FP32 CHW RGB [0,1]
```
R = bgra[src+2] / 255.0f    // B→R swap
G = bgra[src+1] / 255.0f
B = bgra[src+0] / 255.0f    // R→B swap
Output layout: float[3][H][W] CHW
```

### GPU Inference
`cudaMemcpy H→D → enqueueV3 → cudaMemcpy D→H`

### Postprocess
Threshold filter + clamp to `[0, modelW-1] × [0, modelH-1]`. **NO coordinate scaling** — coords stay in model pixel space.

### API
```cpp
bool Initialize(enginePath, modelW=416, modelH=416, numDets=300);
vector<Detection> Infer(const uint8_t* bgra, float confThr=0.25f);
int GetModelWidth/Height() const;

struct Detection {
    float x1, y1, x2, y2;   // model pixel coords
    float confidence;
    int   classId;           // 0=enemy, 1=teammate
};
```

---

## 7. UdpReplySender (Stage 5)

Packs inference results and sends to Host over UDP.

### Wire format

```
┌──────────────┬────────────────────────────────┐
│ ReplyHeader  │     DetectionRaw[]              │
│   16 bytes   │   numDets × 24 bytes             │
└──────────────┴────────────────────────────────┘

ReplyHeader (16 bytes, packed):
  ┌────────┬────────┬────────┬────────────┐
  │ magic  │frameId │numDets │  padding   │
  │  2B    │  4B    │  2B    │    8B      │
  └────────┴────────┴────────┴────────────┘
  magic:   0x5359 ('SY')

DetectionRaw (24 bytes, packed):
  ┌───────┬───────┬───────┬───────┬───────┬───────┐
  │  x1   │  y1   │  x2   │  y2   │ conf  │classId│
  │  f32  │  f32  │  f32  │  f32  │  f32  │  u32  │
  └───────┴───────┴───────┴───────┴───────┴───────┘
```

All fields little-endian (native x64). Max 50 detections per reply.
For 6 detections: 16 + 144 = 160 bytes.

### Behavior
- Non-blocking UDP socket (`FIONBIO`), fire-and-forget
- Sends reply only if detections are non-empty
- Target: Host IP, port 8889 (configurable)
- Silent on `WSAEWOULDBLOCK` (no error print)

---

## 8. Stats Output

```
---- per-second stats --------------------------------
  ROI: 416x416  |  FPS:   170.0  |  frames:   170  |  dropped:     0  |  drop rate:   0.0%
  recv:   0.12 ms  |  infer:   3.52 ms  |  total:   3.64 ms  |  inference:   170/s  |  throughput:   3.45 MB/s
```

| Field | Meaning |
|-------|---------|
| `recv` | UDP drain + reassembly + LZ4 decompress average (ms) |
| `infer` | Preprocess + GPU inference + postprocess average (ms) |
| `total` | recv + infer (ms) |
| `inference` | Infer() calls completed per second |

Detection print every 30 frames:
```
[INFER] Frame #30: 6 detections
  [enemy] conf=0.84 box=[307,36,325,87]
  [enemy] conf=0.72 box=[131,281,143,311]
```

---

## 9. Verification

- **Frame #10**: saved as `client_test.bmp` (32-bit BGRA, top-down DIB)
- **First 4 pixels** printed for manual check against `test_roi.bmp`
- **Drop detection**: Type A (abandoned partial frame) + Type B (frameId gap)
- **ROI mismatch**: one-time warning if Host sends non-416×416

---

## 10. Troubleshooting

| Symptom | Check |
|---------|-------|
| No output at all | TRT/CUDA DLLs in PATH? Run with no engine param first |
| `Error Code 3` | Fixed in latest build (setInput/OutputTensorAddress) |
| `drop rate > 0%` | Socket buffer, cable quality, host sendto errors |
| `LZ4_decompress_safe ERROR` | Corrupted payload, bit-flip |
| `enqueueV3 FAILED` | GPU out of memory or bad engine, rebuild with trtexec |
| `ROI mismatch` warning | Host must send 416×416, or rebuild engine at new size |
| Reply not received | Host must listen on UDP 8889, parse `ReplyHeader` |

---

## 11. Quick Reference

```
┌──────────────────────────────────────────────────────────────────┐
│                 Synapse-X CLIENT — Quick Reference                │
├──────────────────────────────────────────────────────────────────┤
│                                                                   │
│  BUILD:    cd client && cmake --preset windows-x64                │
│            cmake --build build_x64 --config RelWithDebInfo        │
│                                                                   │
│  RUN:      .\build_x64\RelWithDebInfo\SynapseX_Client.exe         │
│              [port=8888] [engine=../../model/bf416.engine]        │
│              [hostIp=192.168.100.1]                               │
│                                                                   │
│  DATA IN:  UDP :8888, 20-byte PacketHeader, magic=0x5358          │
│  REPLY OUT:UDP :8889, 16-byte ReplyHeader, magic=0x5359           │
│                                                                   │
│  MODEL:    416×416 FP16 TRT engine                                │
│  COORDS:   model pixel space [0,415], Host maps to screen        │
│                                                                   │
│  LATENCY:  ~3.5 ms total (recv + infer + reply)                  │
│  FPS:      170 sustained (budget 5.88 ms)                        │
│                                                                   │
└──────────────────────────────────────────────────────────────────┘
```
