# Synapse-X Client Specification

> **Status**: 已交付。全链路跑通，170 FPS 稳态，0% 丢帧。
> **副机 IP**: 192.168.100.2 | **主机 IP**: 192.168.100.1

---

## 1. Pipeline Overview

```
Host → UDP :8888 (20B PacketHeader + LZ4 chunks)
         │
         ▼
  [1] UdpReceiver      0.10–0.25 ms  非阻塞排空 + 乱序重组 + LZ4 解压
  [2] TrtInference     2.80–4.30 ms  BGRA→FP32 CHW → GPU(FP16) → detections
  [3] UdpReplySender   < 0.01 ms     fire-and-forget UDP → Host:8889
         │
         ▼
Host ← UDP :8889 (16B ReplyHeader + DetectionRaw[])
```

**全链路 ~3.0–4.5 ms**，170 FPS 预算 5.88 ms 内，余量充足。

---

## 2. Files

```
client/
├── CMakeLists.txt              LZ4 + CUDA 13.1 + TRT 10.16 + ws2_32
├── CMakePresets.json           VS 2026 x64
├── model/
│   ├── bf416.onnx              通用 ONNX (9.3 MB)
│   └── bf416.engine            编译后引擎 (7.4 MB, GPU/TRT 版本绑定)
├── include/
│   ├── ReassemblyBuffer.h      乱序重组引擎 (header-only struct)
│   ├── UdpReceiver.h           收包 + 解压
│   ├── TrtInference.h          TRT 推理封装
│   └── UdpReplySender.h        回复通道
└── src/
    ├── UdpReceiver.cpp
    ├── TrtInference.cpp
    ├── UdpReplySender.cpp
    └── main.cpp                主循环 + 统计 + BMP

shared/include/
├── PacketHeader.h              20B Host→Client 协议
└── ReplyPacket.h               16B Client→Host 协议
```

---

## 3. Build & Run

```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# 确保 DLL 在 PATH：
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11...\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"

# 默认启动（推理 + 回复，不存图）：
.\build_x64\RelWithDebInfo\SynapseX_Client.exe

# 开启 BMP 存图：
.\build_x64\RelWithDebInfo\SynapseX_Client.exe 8888 ../../model/bf416.engine 192.168.100.1 --save
```

---

## 4. Protocol Summary

### Host → Client (data, port 8888)

```
PacketHeader 20B: magic(2) frameId(4) totalChunks(2) chunkIdx(2)
                  totalSize(4) payloadSize(2) width(2) height(2)
                  + LZ4 payload ≤1400B
```

### Client → Host (reply, port 8889)

```
ReplyHeader 16B: magic(0x5359) frameId(4) numDets(2) padding(8)
                 + DetectionRaw[]. N×24B:
                   x1 y1 x2 y2 conf(f32) classId(u32)
```

坐标在 416×416 模型像素空间。Host 用 `roiOffset + det` 映射到屏幕。

---

## 5. Key Performance

| 指标 | 值 |
|------|-----|
| 网络收包 + 重组 + 解压 | 0.10–0.25 ms |
| TRT 预处理 + GPU 推理 + 后处理 | 2.80–4.30 ms |
| 全链路总延迟 | 3.0–4.5 ms |
| 丢帧率（稳态） | 0% |
| GPU 计算 (trtexec) | ~1.57 ms |
| 网络吞吐 | ~3.4 MB/s @ 170 FPS |

---

## 6. 已交付内容

| 模块 | 状态 |
|------|------|
| UDP 非阻塞收包 + 乱序重组引擎 | 完成 |
| LZ4 raw block 解压 + 动态 ROI 校验 | 完成 |
| TensorRT FP16 推理 (bf416.engine) | 完成 |
| BGRA→FP32 CHW RGB 预处理 | 完成 |
| Client→Host 检测结果回复通道 (UDP 8889) | 完成 |
| 每秒统计 (FPS / 丢帧 / 延迟 / 推理数) | 完成 |
| `--save` 参数控制每秒 BMP 存图 | 完成 |
| CUDA 13.1 + TRT 10.16 CMake 自动寻址 | 完成 |
| `CLIENT_SPEC.md` + `HOST_REPLY_TASK.md` 文档 | 完成 |

---

## 7. 当前问题

| 问题 | 严重度 | 说明 |
|------|--------|------|
| CPU 预处理瓶颈 | 中 | BGRA→FP32 在 CPU 上做双重循环 `/255.0f`，占推理延迟大头 (~2 ms)。416² 还好，更大模型会更明显 |
| `enqueueV3` default stream | 低 | TensorRT 警告用默认 CUDA stream，隐含额外 `cudaStreamSynchronize`。不影响正确性，轻微影响性能 |
| 无帧超时丢弃 | 低 | 如果 Host 停止发帧，Client 会永远卡在当前不完全帧上（直到新 frameId 到达）。生产环境应加 12ms 超时 |
| 解压缓冲区 67 MB 预分配 | 低 | 为 4096² 最坏情况分配，416² 只用 ~700 KB。内存浪费 |
| 推理与收包同线程 | 中 | `TryReceive` 和 `Infer` 串行。如果 GPU 推理时间波动，会影响收包及时性。应分离线程 |
| 回复通道无重试 | 低 | `sendto` 失败只打日志，无重试。UDP 丢回复则 Host 收不到 |
| 坐标无去重/平滑 | 中 | 连续帧的检测结果完全独立，帧间检测框可能抖动。建议加 IoU 跟踪或 EMA 平滑 |

---

## 8. 未来优化方向

### 高优先级
- **预处理搬上 GPU**：用 CUDA kernel 做 BGRA→FP32 CHW，省掉 CPU 上的 416×416 双重循环，预计推理总延迟降到 ~2 ms
- **推理线程分离**：将 `Infer` 移到独立线程，主线程继续收包不等待 GPU，避免背压
- **CUDA stream 复用**：给 `enqueueV3` 传非默认 stream，消除 TensorRT 的 sync warning

### 中优先级
- **帧超时**：给 ReassemblyBuffer 加 12ms 超时，避免 Host 停止时卡死
- **预分配按需调整**：根据实际 ROI 尺寸调整预分配，减少 67 MB 常驻内存
- **坐标跟踪平滑**：对检测结果做帧间 IoU 匹配 + 指数平滑，减少抖动
- **多模型支持**：允许运行时指定 engine 路径和模型尺寸，而非编译期硬编码 416

### 低优先级
- **丢帧策略**：GPU 跟不上时主动丢帧（只处理最新帧），而不是积压
- **回复压缩**：连续帧无检测时不发回复（当前已有：空检测不回复）
- **CUDA Graph**：用 CUDA Graph 固定前处理+推理+后处理的 GPU 执行图，降低 launch 开销

---

## 9. 速查

```
BUILD:  cd client && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
RUN:    .\build_x64\RelWithDebInfo\SynapseX_Client.exe [port] [engine] [hostIp] [--save]
DATA:   UDP :8888 ← Host (20B header, LZ4 chunks)
REPLY:  UDP :8889 → Host (16B header, DetectionRaw[])
MODEL:  416×416 FP16, bf416.engine
DELAY:  ~3.5 ms total
FPS:    170 sustained, 0% drop
```
