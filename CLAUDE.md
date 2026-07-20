# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Synapse-X is an ultra-low-latency dual-machine AI visual inference pipeline with real-time aim assist. A **Host** (gaming PC) captures the screen at 170Hz, compresses frames with LZ4, and sends them over a dedicated Ethernet cable to a **Client** (inference PC) running TensorRT YOLO. The Client returns detection coordinates, and the Host drives mouse movement via `ddll64.dll`.

All comments and documentation are in Chinese. Code identifiers are in English.

## Build Commands

Host and Client are built **independently** on separate physical machines. There is no top-level build.

### Host (gaming machine)
```powershell
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Client (inference machine, requires CUDA + TensorRT)
```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Test program (Host only)
The Host build also produces `SynapseX_Host_TestBmp.exe` — a standalone BMP capture + LZ4 compression test. Built as part of the Host build above.

### Client inference test
```powershell
cd client\test
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```
Produces `test_infer.exe` which runs TensorRT inference against test images (`test/image/*.jpg`) and compares against expected results (`test/result/*_detections.txt`). Links WIC and COM for image loading.

## Architecture

```
Host (gaming PC)                          Client (inference PC)
┌──────────────────────┐                  ┌─────────────────────────┐
│ DXGI → LZ4 → UDP :8888 ────dedicated───→ Reassembly → LZ4 decode │
│                      │     Ethernet     │                         │
│ MouseCtrl ← UDP :8889 ←───────────────── TensorRT YOLO inference  │
│                      │                  │                         │
│ HttpTuner :9999      │                  │                         │
└──────────────────────┘                  └─────────────────────────┘
```

### Shared Protocol (`shared/include/`)

Header-only library used by both sides via relative path `../shared/include/`.

- **`PacketHeader.h`** — Host→Client frame metadata (24 bytes): magic `0x5358`, frameId, chunking info, ROI dimensions, modelId. Max payload 1400 bytes per UDP datagram.
- **`ReplyPacket.h`** — Client→Host detection results: `ReplyHeader` (16 bytes, magic `0x5359`) + up to 50 `DetectionRaw` structs (24 bytes each: x1,y1,x2,y2,confidence,classId).
- **`Log.h`** — spdlog-based logging setup with console + rotating file sinks. Defines `SX_LOG_*` macros (`SX_LOG_TRACE` through `SX_LOG_CRITICAL`). Call `SynapseX::Log::Initialize(appName)` at startup.

### Host Modules (`host/`)

| Module | Header | Role |
|--------|--------|------|
| DxgiCapturer | `include/DxgiCapturer.h` | DXGI Desktop Duplication, GPU-side ROI crop, 170Hz fixed-rate capture, thread pinning to P-Core |
| Lz4Compressor | `include/Lz4Compressor.h` | LZ4 block compression with `LZ4_compress_fast(accel=5)` |
| UdpSender | `include/UdpSender.h` | Chunks compressed frame into ≤1400B packets with PacketHeader, non-blocking 4MB buffer |
| UdpReplyReceiver | `include/UdpReplyReceiver.h` | Listens on UDP :8889, deserializes ReplyHeader + DetectionRaw[], maps coordinates to screen space |
| MouseController | `include/MouseController.h` | PD controller with dynamic Kp, Kd damping, sub-pixel accumulator, 2-frame delay compensation, loads `ddll64.dll` |
| HttpTuner | `include/HttpTuner.h` | Embedded HTTP server on :9999 serving `web/index.html` for real-time parameter tuning from phone/tablet |

### Client Modules (`client/`)

| Module | Header | Role |
|--------|--------|------|
| UdpReceiver | `include/UdpReceiver.h` | UDP recv on :8888, decompresses with LZ4 |
| ReassemblyBuffer | `include/ReassemblyBuffer.h` | Out-of-order packet reassembly by frameId |
| TrtInference | `include/TrtInference.h` | TensorRT YOLO inference, FP16, 416×416, loads `.engine` file |
| CudaPreprocess | `include/CudaPreprocess.h` | GPU-side image preprocessing via NVRTC |
| UdpReplySender | `include/UdpReplySender.h` | Sends detection results back to Host on :8889 |

## Key Architectural Details

### Host Main Loop (170 Hz fixed-rate, `host/src/main.cpp`)

Each tick (~5.882ms) executes these stages in order:

1. **Hotkey polling** — `GetAsyncKeyState` edge-triggered detection for PageUp (enable) / PageDown (disable) aim assist.
2. **DXGI capture** — `DxgiCapturer::CaptureFrame()` with `AcquireNextFrame(timeout=0)`. If no new frame, reuses cached compressed data.
3. **LZ4 compression** — Only when new frame arrives. `LZ4_compress_fast(accel=5)`. Result cached for subsequent re-sends.
4. **UDP send** — Always sends every tick (new frame or cached repeat). `frameId` increments monotonically. Chunks into 1400-byte packets via stack-allocated buffer — zero heap allocation on hot path.
5. **Reply receive** — Non-blocking drain of all queued datagrams on port 8889. Maps model-space coordinates to screen-space via ROI offset.
6. **Target selection + PD aim** — Priority-aware spatial lock system (80px keep radius, 5 max lost frames). Dynamic Kp PD controller.
7. **`sleep_until(nextTick)`** — If behind schedule, resets `nextTick = now` to avoid death spiral.

**Typical latency budget per tick:** capture ~0.15ms, compress ~0.18ms, send ~0.02ms, receive + aim <0.01ms = **~0.35ms total** on Host. Client inference adds ~1.5–1.8ms. End-to-end ~2ms.

### Host Thread Model (`host/src/main.cpp`)

- Main thread pinned to **core 2** (`SetThreadAffinityMask(1ULL << 2)`), `THREAD_PRIORITY_TIME_CRITICAL`.
- `timeBeginPeriod(1)` — Windows timer resolution from 15.6ms to 1ms.
- HttpTuner runs on a separate background thread.

### Client Async Producer-Consumer (`client/src/main.cpp`)

```
Producer (main thread, core 0):         Consumer (std::thread, core 1):
  UDP recv → reassembly → LZ4 decomp      CUDA stream → NVRTC kernel → TRT infer
         ↓                                        ↑
    ┌────┴────┐                             ┌────┴────┐
    │ LIFO slot (size 1, mutex + cv) ───────→ pop     │
    │         │  overwrites stale frames    │         │
    └─────────┘                             └─────────┘
```

- **FrameSlot** (size 1): mutex + condition_variable + `hasNew` flag + `drops` counter. Producer overwrites unconditionally — if consumer hasn't popped the previous frame yet, `drops` increments (LIFO semantics). Consumer waits with 2ms timeout.
- **No GPU work on producer thread.** Zero CUDA API calls from the network thread.
- **Core pinning:** Producer = core 0, Consumer = core 1.
- **Warmup:** Consumer runs 50 frames of black dummy data through `Infer()` at startup to force GPU max P-state and JIT compile TRT kernels.

### Model Hot-Switching

`g_targetModelId` (`std::atomic<uint8_t>`, declared in `PacketHeader.h`, defined in `TrtInference.cpp`):
- **Written by:** UdpReceiver's producer thread from `PacketHeader.modelId` on each new frame.
- **Read by:** TrtInference::Infer() consumer thread at the top of every call.
- **Switch sequence:** `cudaStreamSynchronize` → `UnloadEngine()` (free GPU buffers, delete context/engine) → `LoadEngineFile(targetId)` → return empty detections (discard stale frame).

Model ID mapping (8 games):
| ID | Game | Classes |
|----|------|---------|
| 0 | Apex Legends | 2: teammate (classId=0), enemy (classId=1) — 保留classId=1 |
| 1 | Delta Force | 2: body, head |
| 2 | Battlefield 6 | 2: enemy, teammate |
| 3 | Overwatch 2 | 1: enemy |
| 4 | Aimlabs | 1: enemy |
| 5 | PUBG | 2: body, head |
| 6 | CrossFire | 2: body, head |
| 7 | CS2 | 2: CT (classId=0), T (classId=1) |

### Client GPU Pipeline (`TrtInference::Infer()`)

All operations on a single non-blocking CUDA stream:
1. `cudaMemcpyAsync` BGRA H→D
2. `LaunchBgra8ToFp32ChwRgb()` — NVRTC-compiled GPU kernel: BGRA uint8 → FP32 CHW RGB (B↔R swap, /255.0f normalize)
3. `ctx->enqueueV3(stream)` — TensorRT FP16 inference
4. `cudaStreamSynchronize(stream)` — only blocking point
5. `cudaMemcpy` D→H — read 300 detections × 6 floats
6. Post-process: filter by `confThr` (default 0.25), clamp coords to [0, modelW/H-1]

### NVRTC over nvcc

CUDA 13.1's `cudafe++` is incompatible with VS 2026 (v180) MSVC. The project sidesteps this entirely by using NVRTC (runtime compilation):
- Kernel source is an embedded C++ string in `CudaPreprocess.cpp`.
- Compiled at runtime via `nvrtcCompileProgram()` with flags: `--gpu-architecture=compute_86`, `-std=c++17`, `-use_fast_math`.
- PTX loaded via `cuModuleLoadData`, kernel handle cached via `cuModuleGetFunction`.
- No `.cu` files, no nvcc invocation at build time.

### PD Controller Algorithm (`MouseController::AimAtTarget()`)

Full formula applied every tick:

1. **Delay compensation:** `realDx = dx - sum(sentHistory[0..1].dx)` — subtracts in-flight MoveR commands from a 2-entry ring buffer to account for 1-2 frame visual latency.
2. **Micro deadzone:** `pixelError < 1.5px` → no movement.
3. **Range gate:** `pixelError > cfg.aimRange` → no movement.
4. **Dynamic Kp:** `currentKp = Kp + (kpMax - Kp) * exp(-kpDecay * pixelError)`. Far targets get base Kp (gentle tracking), close targets approach kpMax (magnetism).
5. **D term:** `dError = realError - prevError` — velocity damping.
6. **PD output:** `out = currentKp * realError + Kd * dError`.
7. **Sub-pixel accumulator:** Accumulates fractional output. Only emits integer `MoveR` when `|residual| >= 1.0`. Prevents quantization jitter.
8. **Record history:** Store `(moveX, moveY)` in ring buffer for next frame's delay compensation.

### Target Selection & Spatial Lock (`host/src/main.cpp` main loop)

- **Unified AimPoint:** All model-specific detection formats normalized to `{cx, cy, priority, distance}`.
- **Priority system:** priority=1 (primary target / head) beats priority=2 (body simulated as head).
- **Lock acquisition:** Nearest priority-1 target within `aimRange`. Falls back to priority-2. Sets `isLocked=true`.
- **Lock maintenance:** Any target within 80px (`kKeepLockRadius`) of locked position. Lost after 5 consecutive frames (`kMaxLostFrames` ≈ 29ms). Priority-1 within 80px auto-upgrades from priority-2 lock.
- **Coordinate scaling:** `dx = (target.cx - screenCenterX) * (gameW / screenW)` — maps screen-space error to game-space movement.

### Reassembly Buffer (`ReassemblyBuffer.h`)

- Preallocated to worst case (4096×4096 ROI): ~67 MB data buffer, ~48K entry received-mask.
- Chunks placed at offset `chunkIndex * MAX_PAYLOAD_SIZE` (1400 bytes).
- `receivedMask` bitmask detects duplicates.
- `IsNewerFrameId()` uses `int32_t` cast for uint32_t wraparound safety (~290 days at 170Hz).
- Frame assembly timeout: 12ms (~2× 170Hz period). Stalled incomplete frames are dropped.

### UDP Network Design

- **Non-blocking everywhere.** `FIONBIO` on all sockets. `WSAEWOULDBLOCK` silently drops packets.
- **Host sender:** 4MB `SO_SNDBUF`. Stack-allocated packet buffer (`PacketHeader + 1400 bytes`). `sendto()` drops on buffer full (logs every 100 drops).
- **Client receiver:** 256KB `SO_RCVBUF`. Drains all datagrams per `TryReceive()` call.
- **Reply receiver:** 64KB `SO_RCVBUF`. 2048-byte aligned stack buffer. Drains until `WSAEWOULDBLOCK`.

### DXGI Error Recovery (`DxgiCapturer.cpp`)

- `DXGI_ERROR_ACCESS_LOST` → sleep 100ms → `ReleaseResources()` → `CreateDeviceAndDuplication()`.
- 500ms cooldown (`kRebuildCooldownMs`) prevents rapid retry loops.
- Cold-start rebuild: if `m_duplication` is null but cooldown elapsed, retry every tick.
- Diagnostics: `ProtectedContentMaskedOut` detection, all-zero frame detection (10 consecutive frames triggers warning).

### Web Tuner Panel (`HttpTuner.cpp` + `web/index.html`)

- Backend: `cpp-httplib` on port 9999, routes `GET /`, `GET /api/state`, `POST /api/config`.
- Hand-written JSON serialization — no third-party JSON library.
- Config persistence via browser `localStorage` (preset save/load/delete).
- Polls `/api/state` every 500ms for live stats display.

## Dependencies

- **LZ4 1.10.0** (`thirdparty/lz4-1.10.0/`) — compiled directly from source as a static library
- **cpp-httplib 0.47.0** (`thirdparty/cpp-httplib-0.47.0/`) — single-header HTTP server, included directly
- **spdlog 1.17.0** (`thirdparty/spdlog-1.17.0/`) — header-only logging, included via `SynapseX_Shared` interface target
- **CUDA 13.1 + TensorRT 10.16.1.11** — Client only, hardcoded path in `client/CMakeLists.txt`: `C:/Program Files/NVIDIA/TensorRT-10.16.1.11.Windows.amd64.cuda-13.2/TensorRT-10.16.1.11`
- **Windows SDK 10.0.26100** — D3D11, DXGI, Winsock2, winmm
- **Visual Studio 2026** (v18) — MSVC toolchain, C++17

## Network Topology

- Dedicated Ethernet direct connection (no switch/router)
- Host IP: `192.168.100.1`, Client IP: `192.168.100.2`
- Host → Client: UDP port 8888 (frame data)
- Client → Host: UDP port 8889 (detection results)
- Host HTTP: TCP port 9999 (tuner web panel)

## Runtime

```powershell
# Client first (requires TRT/CUDA DLLs in PATH)
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# Host (requires Administrator for mouse control)
.\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Client debug: --save flag saves one BMP per second (client_0000.bmp, ...)
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1 --save

# Web tuner: http://192.168.100.1:9999
```

The Host copies `mousedll/ddll64.dll` and `web/` to the build output via CMake post-build commands.

## Important Constraints

- The game must run in **borderless window** mode (fullscreen exclusive causes DXGI to return black frames).
- Mouse control requires **Administrator privileges**.
- The `.engine` file path in `client/model/` is a symlink/gitignored — actual engine files are stored in `client/model/engine/`.
- `ddll64.dll` is committed to the repo (`!host/mousedll/ddll64.dll` in `.gitignore` exception) — it's a required runtime dependency, not built from source.
- Spec documents are at `host/HOST_SPEC.md` and `client/CLIENT_SPEC.md`. They are the authoritative technical references for each side of the pipeline.
- For Python scripts (model export, ONNX conversion, etc.), activate the conda environment first: `conda activate yolo26`.
- To regenerate `.engine` files for a different GPU: `trtexec --onnx=xxx.onnx --saveEngine=xxx.engine --fp16`.
- Client requires VS 2022 build tools (v143) for NVRTC host compiler compatibility.

## Language

始终用中文与用户交流。所有注释、文档和交互输出均使用中文。
