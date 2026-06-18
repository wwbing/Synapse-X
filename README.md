# Synapse-X

**Ultra-low latency dual-machine visual inference pipeline with real-time aim-assist.**

Host captures game frames at 170 Hz, LZ4-compresses, sends over isolated Ethernet.
Client runs TensorRT YOLO inference, returns detection coordinates.
Host drives mouse via ddll64.dll for aim-assist. All parameters tunable via web panel.

---

## Architecture

```
┌── HOST (game machine) ───────────────────────────────────────────┐
│                                                                    │
│  GPU (DXGI Dup) → LZ4 → UDP :8888 ──────────────┐                 │
│                                                   │                 │
│  UDP :8889 ←──────────────────────────────────┐   │                 │
│    │  ReplyHeader + DetectionRaw[]             │   │                 │
│    ▼                                          │   │                 │
│  target select → MouseController (ddll64.dll)  │   │                 │
│    aim-assist (head/body, smooth, configurable)│   │                 │
│                                                   │                 │
│  HttpTuner (:9999) ← web panel (phone/tablet)    │                 │
│    real-time param tuning                        │                 │
└───────────────────────────────────────────────┼───┼─────────────────┘
                                                │   │
                                  Ethernet (physically isolated)
                                                │   │
┌── CLIENT (inference machine) ─────────────────┼───┼─────────────────┐
│                                                │   │                 │
│  UDP :8888 recv → reassemble → LZ4 decompress ─┘   │                 │
│                                                     │                 │
│  TensorRT inference (YOLO, 416×416 FP16) ───────────┘                 │
│    UDP :8889 send (ReplyHeader + DetectionRaw[])                     │
└──────────────────────────────────────────────────────────────────────┘
```

| Component | Machine | Role |
|-----------|---------|------|
| `host/` | Game PC | Capture, compress, send, receive replies, aim-assist, web tuner |
| `client/` | Inference PC | Receive, reassemble, decompress, TensorRT inference, send replies |
| `shared/` | Both | Wire protocol headers (PacketHeader, ReplyPacket) |

---

## Features

- **170 Hz fixed capture** — DXGI Desktop Duplication, GPU-side center-ROI crop, thread pinned to P-core
- **LZ4 compression** — `LZ4_compress_fast(accel=5)`, ~3–30% ratio, <0.5 ms under game load
- **UDP fragmentation** — 20-byte PacketHeader + ≤1400B payload, non-blocking 4MB buffer, MTU-safe
- **Configurable ROI** — CLI: `416×416`, `640×640`, or any 64–4096 px square
- **TensorRT inference** — YOLO FP16 on RTX GPU, ~3.5 ms typical
- **Bidirectional UDP** — Host→Client :8888 (frames), Client→Host :8889 (detections)
- **PD aim-assist** — Sub-pixel accumulator + 2-frame delay compensation, ddll64.dll, head/body aim point
- **Web tuning panel** — `http://<host-ip>:9999`, Kp/Kd/aimRange real-time sliders, phone-friendly
- **Anti-degradation** — Core affinity, LZ4 dynamic accel, UDP non-blocking — pipeline survives game load
- **Auto-recovery** — DXGI access lost → full pipeline rebuild, non-blocking

---

## Quick Start

### Prerequisites

- Windows 10/11 x64, Visual Studio 2026, CMake 3.28+
- NVIDIA GPU on Client (CUDA 13.1, TensorRT 10.16)
- Physically isolated Ethernet: Host `192.168.100.1` ↔ Client `192.168.100.2`

### Build

```powershell
# Host (game machine)
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# Client (inference machine)
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### Run

```powershell
# Client — start first
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# Host — Administrator (required for mouse control)
.\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Web tuning panel → open in browser
http://192.168.100.1:9999
```

### CLI reference

```
SynapseX_Host.exe [target_ip] [port] [width] [height]
  defaults: 192.168.100.2  8888   640     640

SynapseX_Client.exe [port] [engine_path] [host_ip] [--save]
  defaults: 8888  bf416.engine  192.168.100.1
```

---

## Key Numbers

| Parameter | Value |
|-----------|-------|
| Host capture rate | **170 Hz** (5.88 ms fixed cadence, thread pinned to P-core, TIME_CRITICAL) |
| Pipeline latency (host) | **~0.35 ms** (idle and in-game, with perf guards) |
| Client inference | **~1.5–1.8 ms** stable (GPU preprocess NVRTC, P-State locked) |
| End-to-end | **~2 ms** capture → inference → aim |
| Network per frame | **30–400 KB** (content-dependent LZ4 accel=5) |
| UDP datagram | ≤1420 bytes (20B header + ≤1400B payload), non-blocking 4MB buffer |
| Aim controller | PD + sub-pixel accumulator + 2-frame delay compensation |
| ROI range | 64×64 to 4096×4096, configurable at runtime |

---

## Directory Structure

```
Synapse-X/
├── README.md
├── .gitignore
├── CMakeLists.txt
│
├── shared/include/
│   ├── PacketHeader.h              ← Host→Client (20B, width/height)
│   └── ReplyPacket.h               ← Client→Host (16B + DetectionRaw[])
│
├── host/
│   ├── include/
│   │   ├── DxgiCapturer.h          GPU ROI capture
│   │   ├── Lz4Compressor.h          LZ4 block compression
│   │   ├── UdpSender.h              UDP fragmentation + send
│   │   ├── UdpReplyReceiver.h       UDP reply listener + coord mapping
│   │   ├── MouseController.h        ddll64.dll loader + aim-assist
│   │   └── HttpTuner.h              Web tuning panel server
│   ├── src/                         Implementation .cpp files + main.cpp
│   ├── test/test_bmp.cpp            Standalone capture test
│   ├── mousedll/ddll64.dll          Mouse control (committed)
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   └── HOST_SPEC.md                 Full host specification + active issues
│
├── client/
│   ├── include/
│   │   ├── ReassemblyBuffer.h       Out-of-order chunk reassembly
│   │   ├── UdpReceiver.h            UDP recv + LZ4 decompress
│   │   └── TrtInference.h           TensorRT inference wrapper
│   ├── src/                         Implementation + main.cpp
│   ├── CMakeLists.txt
│   └── CLIENT_SPEC.md               Full client specification + active issues
│
└── thirdparty/
    ├── lz4-1.10.0/                   LZ4 source (compiled directly)
    └── cpp-httplib-0.47.0/          HTTP server (single header)
```

---

## Specifications

| Document | Covers |
|----------|--------|
| [host/HOST_SPEC.md](host/HOST_SPEC.md) | Pipeline, modules, protocols, CLI, perf optimizations, active issues |
| [host/MOUSE_CONTROL_SPEC.md](host/MOUSE_CONTROL_SPEC.md) | PD controller, sub-pixel accumulator, delay compensation, target selection, tuning |
| [client/CLIENT_SPEC.md](client/CLIENT_SPEC.md) | Reassembly, inference, reply protocol, GPU perf issues |

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Black frames in-game | Switch game to **Borderless Windowed** |
| `[MouseCtrl] OpenDevice FAILED` | Run as **Administrator** |
| No `[Reply]` output | Check firewall UDP 8889; verify Client sends to correct Host IP |
| Web panel not working | Ctrl+Shift+R **force-refresh** browser; verify `http://<host-ip>:9999` |
| Aim overshooting / oscillating | Increase Kd (damping); enable delay compensation (already on) |
| Aim too slow | Increase Kp (proportional); Kp=0.4→0.6 typical for FPS |
| Pipeline latency spikes in-game | Verify thread affinity (core 2); non-blocking UDP enabled; LZ4 accel=5 |
| Client inference spikes 17–27ms | Lock GPU clocks; warm up TRT engine; see CLIENT_SPEC §9 |
| `[UdpSender] packet dropped` | Normal under extreme load — one frame at 170 Hz is invisible |
