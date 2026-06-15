# Synapse-X Host Specification

> The Host captures the desktop center ROI at a fixed 170 Hz, compresses with LZ4,
> sends via UDP to the Client for inference, receives detection results back, and
> drives mouse aim-assist via ddll64.dll.

---

## Pipeline Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           HOST (this machine)                            │
│                                                                          │
│  ┌────────────┐   ┌────────────┐   ┌──────────┐   ┌──────────────────┐ │
│  │ DxgiCapturer│ → │Lz4Compressor│ → │ UdpSender │ → │  UDP :8888       │─│─→ Client
│  │ GPU ROI     │   │ LZ4 fast(1) │   │ stack buf │   │  ≤1420B/packet   │ │
│  │ 0.04 ms     │   │ 0.19 ms     │   │ 0.08 ms   │   │                  │ │
│  └────────────┘   └────────────┘   └──────────┘   └──────────────────┘ │
│                                                                          │
│  ┌──────────────────┐   ┌──────────────────┐                             │
│  │ UdpReplyReceiver │ ← │  UDP :8889        │←── Client reply           │
│  │ map → screen     │   │  ReplyHeader+Det[] │                             │
│  └──────┬───────────┘   └──────────────────┘                             │
│         │ target selection (best enemy by confidence+distance)           │
│         ▼                                                                │
│  ┌──────────────────┐                                                    │
│  │ MouseController   │   MoveR(dx, dy)  smooth approach 15%/frame        │
│  │ ddll64.dll        │   aim range ≤500px, min confidence 0.25           │
│  └──────────────────┘                                                    │
└─────────────────────────────────────────────────────────────────────────┘
```

| Stage | Module | Typical latency |
|-------|--------|-----------------|
| Capture | `DxgiCapturer` — GPU CopySubresourceRegion (center ROI only), Map to RAM | ~0.04 ms |
| Compress | `Lz4Compressor` — `LZ4_compress_fast(accel=1)`, pre-allocated buffer | ~0.19 ms |
| Send | `UdpSender` — stack-allocated header+payload, `sendto()` per chunk | ~0.08 ms |
| Receive reply | `UdpReplyReceiver` — non-blocking drain, model→screen coord mapping | <0.01 ms |
| Aim | `MouseController` — `MoveR` relative movement, exponential decay | <0.01 ms |
| **Total** | | **~0.35 ms** (well under 5.88 ms budget @170 Hz) |

---

## Modules

### DxgiCapturer — GPU Screen Capture

- **API**: DXGI Desktop Duplication (D3D11)
- **ROI**: Center-crop via `CopySubresourceRegion` — only the ROI rectangle is copied to a staging texture, then `Map`'d to system RAM.
- **Format**: BGRA 8-bit (DXGI_FORMAT_B8G8R8A8_UNORM), top-down.
- **Recovery**: Auto-rebuilds the D3D11/duplication pipeline on `DXGI_ERROR_ACCESS_LOST`.
- **Multi-monitor**: Enumerates all outputs, picks the first attached desktop with non-zero area.
- **Limitation**: Cannot capture Exclusive Fullscreen games (game bypasses desktop compositor). Use Borderless Windowed.

### Lz4Compressor — LZ4 Block Compression

- **Function**: `LZ4_compress_fast(src, dst, srcSize, dstCap, acceleration=1)`.
- **Buffers**: Pre-allocated to `LZ4_compressBound(rawSize)` — zero allocation in hot path.
- **Compression ratio**: 2–25% of raw size depending on desktop content.
- **Source**: Third-party LZ4 source compiled directly into the project (`thirdparty/lz4-1.10.0/lib/lz4.c`).

### UdpSender — UDP Fragmentation & Send

- **Protocol**: 20-byte `PacketHeader` + ≤1400-byte LZ4 payload per datagram.
- **Buffer**: Stack-allocated `uint8_t[1420]` — zero heap allocation in send loop.
- **Socket**: 256 KB `SO_SNDBUF`, blocking send (loopback/broadband LAN can't fill it).
- **Chunking**: `totalChunks = ceil(totalSize / 1400)`, offset = `chunkIndex × 1400`.

### UdpReplyReceiver — Client Reply Listener

- **Port**: UDP 8889, non-blocking drain in the main loop.
- **Protocol**: `ReplyHeader` (16 bytes, magic=0x5359) + `DetectionRaw[]` (24 bytes each, FP32).
- **Mapping**: Model-pixel coords → screen coords: `screen = roiOffset + model`.
- **Target selection**: Best enemy (classId=0) by confidence, tie-break by distance to screen center.

### MouseController — Aim-Assist

- **DLL**: `ddll64.dll` — loaded at runtime via `LoadLibrary`/`GetProcAddress`.
- **API**: `MoveR(dx, dy)` — relative mouse movement in pixels.
- **Algorithm**: Exponential-decay approach — move `15% × distance` each frame.
- **Constraints**: Only aim if target within 500px of screen center and confidence ≥ 0.25.
- **Requires**: Administrator privileges (DLL needs elevation to inject input).

---

## Usage

```powershell
# Default: 640×640 to 192.168.100.2:8888
.\SynapseX_Host.exe

# Custom target and ROI (e.g. 416×416 for YOLO models)
.\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Full argument order
.\SynapseX_Host.exe [target_ip] [port] [width] [height]

# Run as Administrator for mouse control
```

ROI constraints: 64–4096 pixels per dimension.

---

## Build

```powershell
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

Requires: CMake 3.28+, Visual Studio 2026 (MSVC 14.51), Windows SDK 10.0.26100+.

Dependencies: D3D11, DXGI, Winsock2 (system). LZ4 compiled from source in `../thirdparty/lz4-1.10.0/`.

---

## Directory Layout

```
host/
├── include/
│   ├── DxgiCapturer.h         GPU desktop duplication capture
│   ├── Lz4Compressor.h         LZ4 block compression
│   ├── UdpSender.h             UDP fragmentation + send
│   ├── UdpReplyReceiver.h      UDP reply listener + coord mapping
│   └── MouseController.h       ddll64.dll loader + aim-assist
├── src/
│   ├── main.cpp                fixed 170 Hz pipeline
│   ├── DxgiCapturer.cpp
│   ├── Lz4Compressor.cpp
│   ├── UdpSender.cpp
│   ├── UdpReplyReceiver.cpp
│   └── MouseController.cpp
├── test/
│   └── test_bmp.cpp            standalone capture+compress test
├── mousedll/
│   └── ddll64.dll              mouse control DLL (committed to repo)
├── CMakeLists.txt
├── CMakePresets.json
└── HOST_SPEC.md                this file
```

---

## Wire Protocols

### Host → Client (UDP :8888)

20-byte `PacketHeader` + LZ4 payload. See `shared/include/PacketHeader.h`.

### Client → Host (UDP :8889)

16-byte `ReplyHeader` + N × 24-byte `DetectionRaw`. See `shared/include/ReplyPacket.h`.

Both protocols are little-endian, `#pragma pack(1)`, with magic bytes for validation.

---

## Tuning

### Aim parameters (in `main.cpp`)

```cpp
AimConfig cfg;
cfg.smoothFactor  = 0.15f;   // fraction of remaining distance per frame
cfg.aimRange      = 500.0f;  // max px from center to engage
cfg.sensitivity   = 1.0f;    // game sensitivity multiplier
cfg.minConfidence = 0.25f;   // ignore low-confidence detections
```

### Frame rate

Fixed 170 Hz via `timeBeginPeriod(1)` + `sleep_until`. Pipeline latency ~0.35 ms means
plenty of headroom. If the pipeline ever exceeds budget, the cadence resets to avoid
death-spiral.

### Desktop idle behavior

When the desktop is static, the Host re-sends the cached compressed frame at 170 Hz.
No re-compression cost — only the cached buffer is re-transmitted. This keeps the
Client's inference pipeline fed with a steady stream.

---

## Known Limitations

1. **Exclusive Fullscreen**: Cannot capture. Switch game to Borderless Windowed.
2. **Anti-cheat**: Some games actively block Desktop Duplication regardless of window mode.
3. **Administrator**: Required for `ddll64.dll` mouse control.
4. **Multi-GPU laptops**: May need to adjust adapter enumeration if capture targets wrong GPU.

---

## Active Issues (observed, needs fixing)

### Host: Aiming Quality

| # | Symptom | Suspected Cause | Fix Direction |
|---|---------|-----------------|---------------|
| A1 | ~~**Aim oscillation**~~ **FIXED** — PD controller + 3px deadzone | Exponential decay had no damping; now D-term provides braking force. Deadzone stops all movement within 3px. | ✅ |
| A2 | ~~**Aim too slow**~~ **FIXED** — P-term provides proportional pull | Old 15%/frame was sluggish at distance. Kp=0.4 gives immediate proportional response; far targets move fast, close targets move slow naturally. | ✅ |
| A3 | **Target switching** — jumps between enemies when confidence values flicker | Best-target selection is purely per-frame: `max(confidence)` with distance tie-break. A slightly higher-confidence detection on the next frame causes an instant switch. | Target lock: once a target is selected, require N consecutive frames of a *better* candidate before switching. Or hysteresis: new target must beat current by a margin (e.g. 0.1 confidence or 50px closer). |
| A4 | **Lock not tight** — crosshair drifts off target during movement | No movement prediction. Target moves between frames but aim always aims at the *previous* frame's position. | Velocity estimation (EMA of position deltas across frames) + lead the target by `velocity * inference_latency`. |

### Host: Aim Smoothing

| # | Symptom | Suspected Cause | Fix Direction |
|---|---------|-----------------|---------------|
| B1 | **Linear decay feels robotic** | `moveX = dx * smoothFactor` produces a straight exponential curve. Human aim has micro-corrections, overshoot, and varying speed. | Add Perlin noise or sinusoidal perturbation at close range. Randomize smoothFactor slightly each frame (±10%). |
| B2 | **No recoil compensation** | Game-specific recoil patterns are not modeled. After firing, crosshair climbs but aim-assist doesn't counteract it. | Per-game recoil table (simple array of (dx, dy) offsets per shot). Subtract from target position after each shot. |

> Client-side performance issues (inference time spikes, GPU contention) are tracked in `client/CLIENT_SPEC.md`.

---

## Unimplemented / Future Work

### Detection & Inference

| Issue | Current | Ideal |
|-------|---------|-------|
| **Detection jitter** | No temporal smoothing — bbox jumps frame-to-frame | Running average or EMA over last 3–5 frames to stabilize bbox corners. |
| **Multi-class priority** | Picks best enemy by confidence+distance | Configurable priority list: e.g. `enemy > teammate` or `closest > highest-conf`. |
| **ROI switching** | Set at startup, requires restart to change | Runtime ROI toggle via hotkey or UDP command. Useful for switching between 416 (inference) and 640 (visual check). |

### Observability & Tuning

| Issue | Current | Ideal |
|-------|---------|-------|
| **Parameter tuning** | Edit `main.cpp` and recompile | Web-based dashboard (localhost HTTP server) to adjust `smoothFactor`, `aimRange`, `sensitivity` in real-time with sliders. |
| **Visual overlay** | Console-only text output | Transparent overlay window showing: capture preview with detection boxes, aim crosshair, FPS graph. |
| **Replay / debug** | No recording | Save raw frames + detections to disk for offline tuning and debugging. |

### Performance

| Issue | Current | Risk |
|-------|---------|------|
| **Idle re-transmit** | Re-sends cached frame at full 170 Hz even when desktop is static | Wastes ~5–50 MB/s on duplicate data. Could add a "skip-if-unchanged" mode that sends only 1 fps keep-alive when idle. |
| **Single-threaded pipeline** | Capture → compress → send → reply → aim all on main thread | If any stage blocks (e.g. GPU Map stalls), the next tick is delayed. Could pipeline capture+compress on a separate thread. |
| **Fixed LZ4 acceleration** | Always `acceleration=1` (fastest, lower ratio) | Could auto-tune: if pipeline has headroom, use higher acceleration for better ratio (less network traffic). |
| **No QoS / prioritization** | All chunks treated equally | If a chunk is lost, the entire frame is dropped. Could add FEC (forward error correction) or retransmit last chunk for critical frames. |
| **GPU contention** | DXGI capture competes with game for GPU time | On GPU-bound games, capture latency may spike. Could lower capture priority or use a secondary GPU for capture. |

### Robustness

| Issue | Current | Ideal |
|-------|---------|-------|
| **UDP no retransmit** | Lost packet = corrupted frame at Client | Optional lightweight NACK-based retransmit for the last N frames. |
| **Client disconnection** | Host keeps sending into void | Detect idle reply stream and alert user. Could auto-pause sending to save bandwidth. |
| **Resolution change** | Handled (ACCESS_LOST auto-rebuild) | Currently works, but would benefit from notifying the Client of the new dimensions. |

### Integration

| Issue | Current | Ideal |
|-------|---------|-------|
| **Hotkey toggle** | Ctrl+C kills everything | Bindable hotkeys: toggle aim on/off, switch target priority, toggle overlay. |
| **Game profiles** | Single hardcoded `AimConfig` | Per-game config files (JSON/TOML) with sensitivity, aim range, class priorities. Auto-detect game by window title. |
| **Multi-monitor game** | Captures output[0] only | Manual output selection via CLI arg `--output 1` for secondary monitor gaming. |
