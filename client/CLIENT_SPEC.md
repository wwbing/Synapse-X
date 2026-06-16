# Synapse-X Client Specification

> **Status**: v3 — GPU 预处理已交付 (NVRTC)。170 FPS 稳态，LIFO 零积压，全链路 ~1.7 ms。
> **副机 IP**: 192.168.100.2 | **主机 IP**: 192.168.100.1

---

## 1. Pipeline Overview (v3 — GPU Preprocess)

```
Host → UDP :8888 (20B PacketHeader + LZ4 chunks)
         │
    ┌────┴──── Producer Thread (core 0) ────────────────────────────┐
    │  [1] UdpReceiver      0.10–0.25 ms                            │
    │      非阻塞排空 + 乱序重组 + LZ4 解压                          │
    │      拼好一帧 → LIFO push（覆盖旧帧，唤醒 Consumer）           │
    └───────────────────────┬───────────────────────────────────────┘
                            │  FrameSlot (size=1, mutex+cv)
    ┌───────────────────────┴───────────────────────────────────────┐
    │  [2] Consumer Thread (core 1, affinity pinned)                │
    │      cudaMemcpyAsync(BGRA H→D)          1.66 MB uint8         │
    │      → GPU kernel (NVRTC): BGRA→FP32 CHW  ~15-30 μs          │
    │      → enqueueV3(stream): TRT FP16         ~1.5 ms            │
    │      → cudaStreamSynchronize              (唯一同步点)         │
    │      → UdpReplySender                     < 0.01 ms           │
    └───────────────────────┬───────────────────────────────────────┘
                            │
Host ← UDP :8889 (16B ReplyHeader + DetectionRaw[])
```

**全链路 ~1.5–1.8 ms**，170 FPS 预算 5.88 ms，余量 > 70%。
PCIe 传输从 FP32 (2.0 MB) 降为 uint8 (1.66 MB)，CPU 预处理循环完全消除。

---

## 2. Environment & Dependencies

### 2.1 Required Software

| Component | Version | Path |
|-----------|---------|------|
| Visual Studio | 2026 (v18) | `C:\Program Files\Microsoft Visual Studio\18\` |
| MSVC toolchain | 14.51 (v180, for C++ code) | 自动检测 |
| MSVC toolchain | 14.44 (v143, for NVRTC host compiler) | 自动检测 |
| CUDA Toolkit | 13.1.115 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1` |
| TensorRT | 10.16.1.11 | `C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11` |
| CMake | ≥ 3.28 | `C:\Program Files\CMake\bin` |

### 2.2 Runtime DLL PATH（必须设置）

```powershell
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"
```

不加这两行会报 `nvinfer_10.dll` 或 `cudart64_131.dll` 找不到。

### 2.3 已知环境兼容性问题

| 问题 | 状态 | 说明 |
|------|------|------|
| **nvcc + VS 2026 不兼容** | 已规避 | CUDA 13.1 的 `cudafe++` 在 VS 2026 (v180) MSVC 上 ACCESS_VIOLATION 崩溃。**解决方案：使用 NVRTC 运行时编译 CUDA kernel，完全不依赖 nvcc 构建。** |
| CMake `enable_language(CUDA)` 失败 | 已规避 | VS 2026 没有 CUDA toolset 注册。同样由 NVRTC 方案规避。 |
| NVRTC 需要 v143 MSVC | **必须** | NVRTC 在运行时用主机编译器编译 CUDA kernel。CUDA 13.1 的 NVRTC 支持 VS 2019-2022 (v142/v143)。已安装 14.44.35207。 |
| LNK4098 (LIBCMT 冲突) | 无害 | NVRTC 静态库用 `/MT`，项目用 `/MD`。链接警告，不影响运行。 |

### 2.4 如果换机器 / 重装系统

按顺序检查：
1. VS 2026 必须安装 **v143 MSVC 工具链**（VS Installer → 单个组件 → "MSVC v143 - VS 2022 C++ x64/x86 build tools"）
2. CUDA 13.1 + TensorRT 10.16 路径是否匹配 CMakeLists.txt 中的 `TRT_BASE_PATH`
3. DLL PATH 是否设好
4. `bf416.engine` 必须在本机用 `trtexec` 重新生成（GPU 绑定）

---

## 3. Architecture Details

### 3.1 Async Producer-Consumer (v2)

```
Producer (main thread, core 0):       Consumer (worker, core 1):
  while g_running:                      PinToCore(1)
    TryReceive() → gotFrame?            cudaSetDevice(0)
      lock → push to FrameSlot          SetupStream()
      notify consumer                   InitCudaPreprocess()  ← NVRTC runtime compile
                                        Warmup 50 frames
  Per-second stats + BMP                while g_running:
                                          wait(cv, 2ms) → pop FrameSlot
                                          Infer(frame) → reply
```

- **FrameSlot**: `mutex` + `condition_variable`，size=1。Producer 覆盖旧帧时 `drops++`
- **消费者超时**: `cv.wait_for(2ms)`，无新帧时周期性唤醒检查 `g_running`
- **绑核**: `SetThreadAffinityMask`，Producer=core0，Consumer=core1

### 3.2 GPU Preprocess via NVRTC (v3)

**为什么用 NVRTC 而不是 nvcc：**
VS 2026 (v18) 的 MSVC 14.51 不在 CUDA 13.1 支持列表中。`-allow-unsupported-compiler` 绕过了版本检查但 `cudafe++` 仍崩溃。NVRTC 运行时编译方案完全绕开了构建期的 nvcc。

**实现文件**：`src/CudaPreprocess.cpp`

```
InitCudaPreprocess():                   // 在 Consumer 线程 init 时调用一次
  nvrtcCreateProgram(kernel_source)     // kernel 源码在 C++ 字符串常量中
  nvrtcCompileProgram(opts)            // --gpu-architecture=compute_86
  nvrtcGetPTX() → cuModuleLoadData()  // 加载 PTX
  cuModuleGetFunction()                // 缓存 CUfunction handle

LaunchBgra8ToFp32ChwRgb(d_bgra, d_fp32, w, h, stream):
  cuLaunchKernel(func, grid, block, 0, stream, args, 0)
  // 16×16 threads, ceil(w/16)×ceil(h/16) blocks
```

**Kernel 功能**：每线程处理一个像素 — 读取 BGRA uint8 → `/255.0f` 归一化 → B↔R 通道交换 → 写入 CHW planar FP32。

**性能**：
- GPU 执行时间: ~15–30 μs（内存带宽受限，not compute bound）
- PCIe 传输: 1.66 MB (uint8) vs 旧方案 2.0 MB (FP32)
- 总计节省: ~2 ms（消除 CPU 双重 for 循环）

### 3.3 CUDA Stream 流水线

整个 GPU 操作在**同一个非默认 CUDA stream** 上排队：
```
cudaMemcpyAsync(BGRA, H→D, stream)
→ LaunchBgra8ToFp32ChwRgb(..., stream)  [GPU kernel]
→ enqueueV3(stream)                     [TRT inference]
→ cudaStreamSynchronize(stream)         [唯一阻塞点]
→ cudaMemcpy(output, D→H)              [读取结果]
```

### 3.4 推理预热

Consumer 线程 init 后跑 50 帧黑图 (`all-zeros BGRA`) 推理：
- 拉升 GPU P-State 到最高频率
- 完成 TRT 引擎的所有 JIT 编译
- `cudaDeviceSynchronize` 确保预热完成后才进入主循环

---

## 4. Files

```
client/
├── CLIENT_SPEC.md               ← this file
├── CMakeLists.txt               LZ4 + CUDA 13.1 + TRT 10.16 + NVRTC + ws2_32
├── CMakePresets.json            VS 2026 x64
├── model/
│   ├── bf416.onnx               通用 ONNX (9.3 MB, GPU 无关)
│   └── bf416.engine             编译后引擎 (7.4 MB, GPU/TRT 版本绑定)
├── include/
│   ├── ReassemblyBuffer.h       乱序重组引擎 (header-only struct)
│   ├── UdpReceiver.h            收包 + 解压
│   ├── TrtInference.h           TRT 推理 (CUDA stream + GPU preprocess)
│   ├── CudaPreprocess.h         GPU kernel 声明 (NVRTC)
│   └── UdpReplySender.h         回复通道
└── src/
    ├── UdpReceiver.cpp          收包 / 重组 / 解压
    ├── TrtInference.cpp         TRT 引擎加载 + Infer (GPU pipeline)
    ├── CudaPreprocess.cpp       NVRTC 编译 + Driver API 启动
    ├── UdpReplySender.cpp       打包回复 + sendto
    └── main.cpp                 双线程主循环 + LIFO + 绑核 + 统计

shared/include/
├── PacketHeader.h               20B Host→Client 协议
└── ReplyPacket.h                16B Client→Host 协议
```

---

## 5. Build & Run

```powershell
# === 一次性：DLL PATH ===
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"

# === 构建 ===
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# === 运行 ===
.\build_x64\RelWithDebInfo\SynapseX_Client.exe
# 参数: [port=8888] [engine=../../model/bf416.engine] [hostIp=192.168.100.1] [--save]
```

### 启动日志（正常）

```
============================================
  Synapse-X Client -- Async Pipeline
  Architecture: Producer(core0) | Consumer(core1)
  Listening on: 0.0.0.0:8888
  Engine path:  ../../model/bf416.engine
  Reply to:     192.168.100.1:8889
  BMP dump:     OFF
============================================

[UdpReceiver] Ready -- listening on 0.0.0.0:8888, ...
[TrtInference] Loaded engine: ../../model/bf416.engine (7.4 MB)
[TrtInference] Ready. Model: 416x416, 300 dets, ... (GPU preprocess via NVRTC)
[UdpReplySender] Ready -- sending to 192.168.100.1:8889
[CPU] Thread pinned to core 0
[INFO] Spawning consumer thread on core 1...
[CPU] Thread pinned to core 1
[TrtInference] CUDA stream created (non-blocking).
[CudaPreprocess] NVRTC build log:
...
[CudaPreprocess] Kernel compiled. PTX: XXXX bytes
[CudaPreprocess] Ready. Kernel compiled at runtime via NVRTC.
[CONSUMER] Warming up GPU (50 dummy frames)...
[CONSUMER] Warmup complete.
[INFO] Producer on core 0. Waiting for Host data...
```

---

## 6. Stats Output

```
---- per-second stats --------------------------------
  ROI: 416x416  |  FPS:   170.0  |  fr:   170  |  drop:     0 ( 0.0%)  |  LIFO drops: 0
  recv:   0.12 ms  |  infer:   1.58 ms  |  total:   1.70 ms  |  infer/s:   170  |  BW:   3.45 MB/s
```

| Field | Meaning |
|-------|---------|
| `recv` | UDP + 重组 + LZ4 解压 (Producer) |
| `infer` | cudaMemcpyAsync + GPU kernel + TRT + sync + 后处理 (Consumer) |
| `total` | recv + infer，全链路单帧 |
| `LIFO drops` | GPU 跟不上被覆盖的帧数 |

---

## 7. Key Performance

| 指标 | 值 |
|------|-----|
| 收包+重组+解压 | 0.10–0.25 ms |
| BGRA H→D copy | ~0.02 ms (PCIe Gen4) |
| GPU kernel (BGRA→FP32) | ~0.02 ms |
| TRT enqueueV3 (FP16) | ~1.5 ms (GPU compute) |
| postprocess + reply | ~0.05 ms |
| **全链路总延迟** | **~1.5–1.8 ms** |
| 丢帧率 (稳态) | 0% |
| 170 FPS 预算余量 | > 70% |
| NVRTC init + warmup | ~5–10 秒（仅启动时） |

---

## 8. Regenerating the TRT Engine

`.engine` 绑定 GPU 型号 + CUDA/TensorRT 版本。换机器或升级驱动后必须重建：

```powershell
& "C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin\trtexec.exe" `
    --onnx=client\model\bf416.onnx `
    --saveEngine=client\model\bf416.engine `
    --fp16
```

ONNX 文件 (`bf416.onnx`) 是 GPU 无关的，保留作为规范模型文件。

---

## 9. Troubleshooting

| 症状 | 原因 | 解决 |
|------|------|------|
| 启动无任何输出 | TRT/CUDA DLL 不在 PATH | 检查 §2.2 的 PATH 设置 |
| `nvinfer_10.dll` 找不到 | TensorRT bin 不在 PATH | 加上 `TRT/bin` 到 PATH |
| `cudart64_131.dll` 找不到 | CUDA bin 不在 PATH | 加上 `CUDA/v13.1/bin` 到 PATH |
| `[CudaPreprocess] nvrtcCompileProgram FAILED` | NVRTC 找不到 v143 MSVC | 安装 VS 2022 build tools (v143) |
| `[TrtInference] deserializeCudaEngine FAILED` | engine 文件损坏或 GPU 不匹配 | 重做 §8 engine 重建 |
| `drop rate > 0%` | 网络丢包或 socket buffer 不够 | 检查网线 / 加大 `SO_RCVBUF` |
| 推理尖刺 3→27ms | GPU P-State 波动 / DPC 延迟 | `nvidia-smi -lgc 2500` 锁频率 |
| LNK4098 警告 | `/MT` vs `/MD` CRT 冲突 | 无害，忽略 |

---

## 10. 已知问题

| # | 问题 | 严重度 | 说明 |
|----|------|--------|------|
| I1 | 推理延迟偶发尖刺 | 中 | GPU P-State 波动或 driver DPC。锁 GPU 频率可缓解 |
| I2 | 解压缓冲区 67 MB 预分配 | 低 | 为 4096² 分配，416² 只用 ~700 KB |
| I3 | 回复通道无重试 | 低 | UDP fire-and-forget |
| I4 | 坐标无帧间平滑 | 中 | 加 IoU 匹配 + EMA |
| I5 | VS 2026 + nvcc 不兼容 | 已规避 | NVRTC 方案，不受影响 |

## 11. Future

- **锁 GPU 频率** — `nvidia-smi -lgc 2500` 消除 P-State 尖刺
- **CUDA Graph** — 固定 pre+infer+post 执行图
- **坐标平滑** — 帧间 IoU + EMA
- **按需预分配** — 根据实际 ROI 调整 67 MB 缓冲区

---

## 12. Quick Reference

```
BUILD:    cd client && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
RUN:      .\build_x64\RelWithDebInfo\SynapseX_Client.exe [port] [engine] [hostIp] [--save]
DLL PATH: TRT\bin + CUDA\v13.1\bin
ARCH:     Producer(core0) → LIFO(1) → Consumer(core1)
PREPROC:  GPU via NVRTC (runtime compile, no nvcc needed)
DATA IN:  UDP :8888 (20B PacketHeader + LZ4 chunks)
REPLY:    UDP :8889 → Host (16B ReplyHeader + DetectionRaw[])
MODEL:    416×416 FP16, bf416.engine
DELAY:    ~1.5–1.8 ms
FPS:      170 sustained, 0% drop
```
