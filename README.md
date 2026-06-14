# Synapse-X

**Ultra-low latency dual-machine visual inference pipeline.**

A Host machine captures desktop frames at 170 Hz, compresses them with LZ4,
sends them over a physically isolated Ethernet link to a Client machine that
runs TensorRT inference and returns detection coordinates for real-time aim-assist.

---

## Architecture

```
┌── HOST (game machine) ──────────────────────────────────────────┐
│                                                                   │
│  GPU (DXGI Dup) → LZ4 → UDP :8888 ──────────────────┐            │
│                                                      │            │
│  UDP :8889 ←─────────────────────────────────────┐   │            │
│    │  detection coords                            │   │            │
│    ▼                                             │   │            │
│  target select → ddll64.dll MoveR()              │   │            │
│    aim-assist                                    │   │            │
└──────────────────────────────────────────────────┼───┼────────────┘
                                                   │   │
                                     Ethernet (physically isolated)
                                                   │   │
┌── CLIENT (inference machine) ────────────────────┼───┼────────────┐
│                                                   │   │            │
│  UDP :8888 recv → LZ4 decompress → BGRA frame ────┘   │            │
│                                                        │            │
│  TensorRT inference (416×416 YOLO) ────────────────────┘            │
│    UDP :8889 send (ReplyHeader + DetectionRaw[])                    │
└─────────────────────────────────────────────────────────────────────┘
```

| Component | Machine | Role |
|-----------|---------|------|
| `host/` | Game PC | Capture, compress, send, receive replies, aim-assist |
| `client/` | Inference PC | Receive, decompress, TensorRT inference, send replies |
| `shared/` | Both | Protocol headers (PacketHeader, ReplyPacket) |

---

## Quick Start

### Prerequisites

- Windows 10/11 x64, Visual Studio 2026, CMake 3.28+
- NVIDIA GPU on Client (CUDA 13.1, TensorRT 10.16)
- Physically isolated Ethernet link between Host and Client
- Host: 192.168.100.1 | Client: 192.168.100.2

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

### Run (two machines)

```powershell
# Client machine — start first
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# Host machine — run as Administrator for mouse control
.\SynapseX_Host.exe 192.168.100.2 8888 416 416
```

### Run (single machine loopback test)

```powershell
# Terminal 1
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 127.0.0.1

# Terminal 2 (Admin)
.\SynapseX_Host.exe 127.0.0.1 8888 416 416
```

---

## Key Numbers

| Parameter | Value |
|-----------|-------|
| Host capture rate | **170 Hz** (fixed, 5.88 ms cadence) |
| Pipeline latency | **~0.35 ms** (capture + compress + send) |
| Client inference | **~3.5 ms** (GPU, fp16 YOLO) |
| End-to-end latency | **~20 ms** (capture → inference → aim response) |
| Network bandwidth | ~5–50 MB/s (content-dependent LZ4 ratio) |
| UDP MTU-friendly | ≤1420 bytes per datagram |

---

## Directory Structure

```
Synapse-X/
├── README.md                     ← this file
├── .gitignore
├── CMakeLists.txt                ← root IDE context
├── shared/
│   └── include/
│       ├── PacketHeader.h        ← Host→Client wire protocol
│       └── ReplyPacket.h         ← Client→Host reply protocol
├── host/
│   ├── include/                  ← public headers
│   │   ├── DxgiCapturer.h
│   │   ├── Lz4Compressor.h
│   │   ├── UdpSender.h
│   │   ├── UdpReplyReceiver.h
│   │   └── MouseController.h
│   ├── src/                      ← implementation
│   ├── test/
│   ├── mousedll/ddll64.dll       ← mouse control DLL
│   ├── CMakeLists.txt
│   └── HOST_SPEC.md              ← detailed Host specification
├── client/
│   ├── include/
│   ├── src/
│   ├── CMakeLists.txt
│   └── CLIENT_SPEC.md            ← detailed Client specification
└── thirdparty/
    └── lz4-1.10.0/               ← LZ4 source (compiled directly)
```

---

## Specifications

| Document | Audience | Content |
|----------|----------|---------|
| [host/HOST_SPEC.md](host/HOST_SPEC.md) | Client developers | Protocol, pipeline, modules, tuning |
| [client/CLIENT_SPEC.md](client/CLIENT_SPEC.md) | Host developers | Inference, reassembly, reply protocol |

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| Black frames in-game | Switch game to **Borderless Windowed** mode |
| `[MouseCtrl] OpenDevice FAILED` | Run Host as **Administrator** |
| No `[Reply]` output | Check firewall allows UDP 8889 on Host |
| FPS drops below 170 | Check network cable; reduce ROI size |
