# Synapse-X Host — Architecture & Implementation Guide

> **Version**: 2026-06-19 (final)  
> **Target audience**: New architects / engineers onboarding the Host codebase.  
> **Covers**: Pipeline, every module, config, Web UI, tuning, env pitfalls.

---

## 1. System Context

```
HOST (game PC, Windows 10/11)                    CLIENT (inference PC, Windows + NVIDIA GPU)
══════════════════════════════                   ═══════════════════════════════════════════
                                                         
DXGI → LZ4 → UDP :8888 ────────── Ethernet ────→ UDP recv → LZ4 decompress → TRT inference
                                                   │
UDP :8889 ←──────────────── Ethernet ──────── UDP reply (ReplyHeader + DetectionRaw[])
  │
  ├─ UdpReplyReceiver (model→screen mapping)
  ├─ Spatial target lock (anti-ping-pong)
  ├─ Auto-stretch compensation (gameW/nativeW)
  ├─ PD controller + sub-pixel accumulator + delay compensation
  └─ ddll64.dll MoveR(dx, dy)

HttpTuner :9999 ──→ Web panel (phone/tablet/any browser)
```

---

## 2. Build Environment

### Required

| Component | Version | Notes |
|-----------|---------|-------|
| CMake | ≥ 3.28 | From `C:\Program Files\CMake\bin` |
| Visual Studio | 2026 (v18) | MSVC 14.51 toolchain |
| Windows SDK | 10.0.26100+ | Bundled with VS |
| LZ4 | Source in `thirdparty/lz4-1.10.0/` | Compiled directly, no prebuilt |

### Build

```powershell
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Environment Pitfalls Encountered

| # | Pitfall | Symptom | Solution |
|---|---------|---------|----------|
| 1 | **MinGW LZ4 prebuilt with MSVC** | `___chkstk_ms` linker error | Don't use prebuilt `lz4_win64_*.lib` — compile `lz4.c` directly from source |
| 2 | **CMake 4.x + old cmake_minimum_required** | LZ4 FetchContent fails with "Compatibility with CMake < 3.5 removed" | Use `cmake_minimum_required(VERSION 3.28)`, set `CMAKE_POLICY_VERSION_MINIMUM=3.5` |
| 3 | **httplib OpenSSL dependency** | `#include <openssl/err.h>` not found | Do NOT `#define CPPHTTPLIB_OPENSSL_SUPPORT`. Just don't define it at all — `#ifdef` skips the block |
| 4 | **VS 2026 generator name** | CMake can't find generator | Use `"Visual Studio 18 2026"` with `"x64,version=10.0.26100"` |
| 5 | **x64 host tools** | MSBuild picks wrong architecture | `CMakePresets.json`: set `"toolset": {"value": "host=x64"}` |
| 6 | **Web UI served stale** | Browser caches old `index.html` | Server reads file from disk each GET; Ctrl+Shift+R forces refresh |

---

## 3. Directory Layout

```
host/
├── include/
│   ├── DxgiCapturer.h           GPU desktop duplication capture (D3D11 + DXGI)
│   ├── Lz4Compressor.h           LZ4 block compression (accel=5, pre-allocated)
│   ├── UdpSender.h               UDP fragmentation + send (non-blocking, 4MB buf)
│   ├── UdpReplyReceiver.h        UDP reply listener (port 8889, non-blocking drain)
│   ├── MouseController.h         PD controller + sub-pixel + delay-comp + aim config
│   └── HttpTuner.h               Embedded HTTP server (port 9999, background thread)
│
├── src/
│   ├── main.cpp                  Fixed 170 Hz pipeline + target selection + auto-stretch
│   ├── DxgiCapturer.cpp          GPU ROI capture, ACCESS_LOST auto-recovery
│   ├── Lz4Compressor.cpp         LZ4_compress_fast wrapper
│   ├── UdpSender.cpp             sendto() fragment loop, stack buffer
│   ├── UdpReplyReceiver.cpp      ReplyHeader parser, model→screen coord mapping
│   ├── MouseController.cpp       PD controller core + sub-pixel accumulator
│   └── HttpTuner.cpp             httplib server + JSON API + file serving
│
├── web/
│   └── index.html                Control panel frontend (served from disk)
│
├── test/
│   └── test_bmp.cpp              Standalone capture + compress test
│
├── mousedll/
│   └── ddll64.dll                Mouse input DLL (committed to repo)
│
├── CMakeLists.txt                 Standalone CMake project
├── CMakePresets.json              VS 2026 x64 preset
├── HOST_SPEC.md                   External protocol + module reference
├── MOUSE_CONTROL_SPEC.md          Mouse control deep-dive
└── HOST_REPLY_TASK.md             Client→Host reply protocol spec
```

---

## 4. Module Deep-Dive

### 4.1 DxgiCapturer — GPU Screen Capture

**API**: DXGI Desktop Duplication (D3D11)  
**What it does**: Captures the desktop, extracts only the center ROI on the GPU via `CopySubresourceRegion`, maps it to system RAM via a staging texture.

**Key design**:
- ROI is always centered on screen: `roiLeft = (screenW - roiW) / 2`
- ComPtr RAII on all D3D11/DXGI objects — zero manual Release()
- Multi-monitor: enumerates all outputs, picks first `AttachedToDesktop` with non-zero area
- Auto-recovery: on `DXGI_ERROR_ACCESS_LOST` (Alt+Tab, resolution change), full pipeline rebuild
- Output format: BGRA 8-bit (DXGI_FORMAT_B8G8R8A8_UNORM)

**Critical pitfall**: Missing `D3D11_CREATE_DEVICE_BGRA_SUPPORT` causes all-zero frames. Must be set at device creation.

**Limitation**: Cannot capture Exclusive Fullscreen games. Game must be in Borderless Windowed.

---

### 4.2 Lz4Compressor — LZ4 Block Compression

**API**: `LZ4_compress_fast(src, dst, srcSize, dstCap, acceleration)`  
**Design**: Pre-allocates `LZ4_compressBound(rawSize)` worth of buffer at init. Hot path is `Compress()` which wraps `LZ4_compress_fast` with zero allocations.

**Key params**:
- `acceleration=5` — trades ~5% compression ratio for ~50% CPU reduction. Critical for high-entropy game frames (grass, particles).
- Default input: `roiW × roiH × 4` bytes (e.g. 416×416×4 = 692,224 bytes)

**Source**: `thirdparty/lz4-1.10.0/lib/lz4.c` compiled directly — no FetchContent, no prebuilt .lib.

---

### 4.3 UdpSender — UDP Fragmentation & Send

**Protocol**: 24-byte `PacketHeader` (from `shared/include/PacketHeader.h`) + ≤1400-byte LZ4 payload = ≤1424 bytes/datagram.

**Header fields**: magic, frameId, totalChunks, chunkIndex, totalSize, payloadSize, width, height, modelId, padding[3].

**Key design**:
- Stack-allocated `uint8_t buf[sizeof(PacketHeader) + 1400]` — zero heap in send loop
- Non-blocking socket (`FIONBIO`) — `sendto()` never stalls the main loop
- 4MB `SO_SNDBUF` — absorbs ~80 frames of burst at 170 Hz
- If buffer full → packet dropped, frame lost, pipeline continues. One dropped frame at 170 Hz is invisible.

**Chunk formula**: `totalChunks = ceil(totalSize / 1400)`, offset = `chunkIndex × 1400`.

---

### 4.4 UdpReplyReceiver — Client Reply Listener

**Port**: UDP 8889, non-blocking drain in main loop.  
**Protocol**: `ReplyHeader` (16B, magic=0x5359, frameId, numDets) + N × `DetectionRaw` (24B each: x1,y1,x2,y2 FP32, conf FP32, classId u32).

**Key design**:
- Parses replies in a while-loop drain (non-blocking `recvfrom`)
- Maps model-pixel coords → screen coords: `screenX = roiOffsetX + modelX`
- `roiOffsetX = (screenW - roiW) / 2`

---

### 4.5 MouseController — PD Aim-Assist

**Architecture**:
```
Input: dx, dy (pixel error from screen center)
  │
  ├─ 1. Delay compensation: realDx = dx − sum(sentMoves[0..1])
  │     (subtracts in-flight MoveR values not yet visible in capture)
  │
  ├─ 2. Deadzone: |realError| < 3px → stop, reset state
  │
  ├─ 3. Range gate: |realError| > aimRange → don't engage
  │
  ├─ 4. PD core: Output = Kp × realError + Kd × (realError − prevError)
  │     (no dt division — fixed 170 Hz makes dt constant)
  │
  ├─ 5. Sub-pixel accumulator: residual += Output
  │     Only when |residual| ≥ 1.0: extract int part, call MoveR
  │     Fractional remainder carries to next frame
  │
  └─ 6. Record sent move in ring buffer for future delay compensation
```

**Config** (`AimConfig`):
| Field | Default | Range | Purpose |
|-------|---------|-------|---------|
| `Kp` | 0.26 | 0.05–1.50 | P-term: pull speed |
| `Kd` | 0.05 | 0.00–0.50 | D-term: braking force |
| `aimRange` | 500 | 50–1000 | Engagement radius (px) |
| `minConfidence` | 0.25 | 0–1 | Ignore below this |
| `aimPoint` | 0 | 0/1 | 0=body center, 1=head |
| `headOffset` | 0.20 | 0.05–0.25 | Head position (bbox top %). **Delta+Head: slider hidden** — real head, no offset needed |
| `gameW/gameH` | 3840/2160 | dropdown | Game resolution for auto-stretch |
| `nativeW/nativeH` | 3840/2160 | fixed | Monitor native res |
| `modelId` | 0 | 0/1 | Model selector (0=416, 1=640) |

**DLL**: `ddll64.dll` loaded at runtime via `LoadLibrary`/`GetProcAddress`. API: `OpenDevice()`, `MoveR(dx, dy)`. Requires Administrator privileges.

---

### 4.6 HttpTuner — Web Tuning Panel

**Port**: 9999 (TCP), background thread via `httplib.h`  
**Serves**: `web/index.html` from disk (edit without recompiling)  
**API**:
- `GET /` → HTML panel
- `GET /api/state` → JSON: full config + stats + target info
- `POST /api/config` → update config fields (web slider → C++ mutex-protected state)

**Thread safety**: `TuningState` protected by `std::mutex`. Main loop calls `GetConfig()` (returns copy), web thread writes via POST handler.

**Dependency**: `thirdparty/cpp-httplib-0.47.0/httplib.h` (MIT, single header). Must NOT define `CPPHTTPLIB_OPENSSL_SUPPORT`.

---

### 4.7 Main Loop — Fixed 170 Hz Pipeline

```
Each tick (5.88 ms):
  ├─ Capture: capturer.CaptureFrame(rawBuffer)
  │     new frame? → compress + update cache
  │     no change? → reuse cached compressed buffer
  │
  ├─ Send: sender.SendCompressedFrame(cached, frameId, w, h)
  │
  ├─ Receive reply: replyReceiver.ReceiveReplies(detections, replyFrameId)
  │     ├─ Spatial lock: Phase A (maintain) / Phase B (acquire)
  │     ├─ Target select: best enemy by confidence+distance
  │     ├─ Auto-stretch: autoScale = gameW / screenW
  │     ├─ dx = (targetCx - screenCx) × autoScale
  │     └─ MouseController.AimAtTarget(dx, dy, ...)
  │
  └─ Per-second stats → stderr + HttpTuner
```

**Performance guards**:
- Thread pinned to core 2 via `SetThreadAffinityMask`
- Priority `THREAD_PRIORITY_TIME_CRITICAL`
- `timeBeginPeriod(1)` — 1ms timer resolution (default is 15.6ms)

**Data Normalizer** — converts raw detections to `AimPoint{cx, cy, priority, distance}`:

| modelId | Game | Head source | Confidence rule | Priority |
|---------|------|-------------|-----------------|----------|
| 0,3 | Apex/OW2 | Computed (`y1 + bh×headOffset`) | Must pass `minConfidence` | 1 |
| 1 | Delta | classId=1 real head; classId=0 body→faked head | **Head bypasses**; body filtered | 1=real, 2=faked |
| 2 | BF6 | Computed (`y1 + bh×headOffset`) | Pass `minConfidence`; teammate dropped | 1 |

**Priority-Aware Spatial Lock** — tracks `lockedPriority` (1 or 2):

Phase A (Maintain, 80px radius): pri=1 nearest → maintain/upgrade; no pri=1 but pri=2 → downgrade (keep lock); neither → lostFrames, unlock after 5.  
Phase B (Acquire, global): pri=1 nearest within `aimRange` → lock; no pri=1 → nearest pri=2.

**Auto-stretch compensation**:
- Web dropdown picks game resolution (e.g. 2880×2160 on 4K monitor)
- `autoScaleX = gameW / nativeW` (e.g. 0.75)
- Applied as `dx = rawDx × autoScaleX` before PD controller

---

## 5. Data Flow Summary

```
1. DXGI capture        → rawBuffer (BGRA, roiW×roiH×4 bytes)
2. LZ4 compress         → compressedBuffer (3-30% of raw, ~0.2ms)
3. UDP fragment & send  → N × (24B PacketHeader + ≤1400B payload)
                           PacketHeader carries: frameId, totalChunks, chunkIdx,
                           totalSize, width, height, modelId
4. UDP reply receive    → ReplyHeader + DetectionRaw[] (port 8889)
5. Coordinate mapping   → model→screen: screen = roiOffset + model
6. Spatial lock         → maintain or acquire target
7. Auto-stretch         → dx *= gameW/screenW
8. PD controller        → Output = Kp·error + Kd·dE
9. Sub-pixel accumulate → |residual|≥1 → MoveR(int)

Repeat at 170 Hz (5.88ms cadence)
```

---

## 6. Web Panel Reference

```
http://<host-ip>:9999

┌──────────────────────────────────┐
│  AIM PARAMETERS                  │
│  Kp (Proportional)  [──●──] 0.26│
│  Kd (Damping)       [─●───] 0.05│
│  Aim Range          [──●──] 500 │
│  Min Confidence     [──●──] 0.25│
│  Aim Point     [Body ▼]         │
│  Head Offset       [──●──] 0.12 │
│  Game Resolution                 │
│  ┌──────────────────────────┐   │
│  │ 3840x2160 (16:9 native) │   │
│  └──────────────────────────┘   │
│  Model                           │
│  ┌──────────────────────────┐   │
│  │ 0 — 416×416 (default)   │   │
│  └──────────────────────────┘   │
│  [✓] Aim Enabled                │
│──────────────────────────────────│
│  PIPELINE STATS                  │
│  Send FPS  170  Capture FPS  45 │
│  Pipeline 0.35ms  Compress      │
│  Fresh 45  Cache 125            │
│──────────────────────────────────│
│  Target: enemy conf=0.84 dist=234│
└──────────────────────────────────┘
```

---

## 7. Run Commands

```powershell
# Build
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# Run (Administrator required for mouse control)
.\build_x64\RelWithDebInfo\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Web panel
http://localhost:9999

# Firewall (if Client can't access web panel)
netsh advfirewall firewall add rule name="SynapseX Web Tuner" dir=in action=allow protocol=TCP localport=9999
```

---

## 8. Tuning Quickstart

```
1. Kd=0, tune Kp: 取"刚好不晃"的最大值 (default 0.26 works for most FPS)
2. Add Kd in 0.01 steps: 取"刚好不晃"的最小值 (default 0.05)
3. aimRange: set to your engagement distance
4. Game Resolution dropdown: match your game's actual rendering resolution
5. minConfidence: 0.25 default, lower = more targets but more false positives
```

---

## 9. Key Files for Newcomers

| Read first | File |
|------------|------|
| Architecture overview | `host/HOST_SPEC.md` |
| This guide | `host/HOST_ARCHITECTURE.md` |
| Mouse control details | `host/MOUSE_CONTROL_SPEC.md` |
| Wire protocols | `shared/include/PacketHeader.h`, `shared/include/ReplyPacket.h` |
| Project status | `STATUS.md` |
| Root README | `README.md` |
