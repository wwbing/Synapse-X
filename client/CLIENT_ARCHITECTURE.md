# Synapse-X Client — Architecture & Onboarding Guide

> 写给新加入的架构师/工程师。读完本文即可理解客户端全貌、数据流、构建环境和所有踩过的坑。

---

## 1. 系统定位

客户端运行在副机（192.168.100.2），与主机（192.168.100.1）通过直连网线物理隔离。
职责：接收 UDP 压缩视频帧 → 解压 → GPU 推理 → 检测结果回传主机。

```
主机 (192.168.100.1)                    副机 (192.168.100.2)
  DXGI 截屏 → LZ4 压缩                    UDP :8888 收包
  → 分片 → UDP :8888 ──── 网线 ────→      → 乱序重组 → LZ4 解压
                                          → TRT GPU 推理 (416×416 FP16)
  UDP :8889 ←── 网线 ←──────────────      → 检测坐标回传 :8889
```

---

## 2. 流水线演进

### v1 — 单线程串行（已废弃）
```
while(1) { TryReceive() → Infer() → reply }
```
问题：GPU 推理偶发 35ms 尖刺时，UDP socket buffer 溢出，丢帧 80%+。

### v2 — 异步双线程 + LIFO 队列（当前骨架）
```
Producer (core 0)          Consumer (core 1)
  TryReceive() → LIFO push   LIFO pop → Infer() → reply
```
Producer 永不等 GPU。队列 size=1，新帧覆盖旧帧，消灭历史积压。

### v3 — GPU 预处理（当前推理层）
CPU 上的 BGRA→FP32 CHW 双重 for 循环换成 GPU kernel（NVRTC 运行时编译）。
PCIe 传输从 FP32 (2.0 MB) 降为 uint8 (1.66 MB)，CPU 预处理 ~2ms 降为 GPU ~20μs。

### v4 — Engine 热切换（当前协议层）
主机在 PacketHeader 中下发 `modelId`（0–3），副机 Consumer 线程在每帧推理前检查。
若 ID 变化：同步 CUDA stream → 销毁旧 engine/context/GPU buffer → 加载新 .engine 文件 → 返回空检测。下一帧即用新模型推理。全程 LIFO 队列不受影响。

---

## 3. 模块地图

```
client/
├── CLIENT_ARCHITECTURE.md       ← 本文
├── CLIENT_SPEC.md               ← 协议/构建/性能速查
├── CMakeLists.txt               ← 主构建 (LZ4 + CUDA 13.1 + TRT 10.16 + NVRTC + ws2_32)
├── CMakePresets.json            ← VS 2026 x64 预设
│
├── model/
│   ├── onnx/                    ← 4 个 ONNX 模型 (GPU 无关，可移植)
│   │   ├── delta_body_head_416.onnx      delta 游戏，body+head 2类
│   │   ├── bf6_enemy_self_416.onnx       bf6 游戏，enemy+self 2类
│   │   ├── apex_enemy_416.onnx           apex 游戏，enemy 1类
│   │   └── ow2_enemy_416.onnx            ow2 游戏，enemy 1类
│   └── engine/                  ← 编译后引擎 (GPU/TRT 版本绑定，每台机器必须重建)
│       ├── delta_body_head_416.engine
│       ├── bf6_enemy_self_416.engine
│       ├── apex_enemy_416.engine
│       └── ow2_enemy_416.engine
│
├── include/
│   ├── ReassemblyBuffer.h       ← 乱序重组引擎 (纯 C++ struct，~140 行)
│   ├── UdpReceiver.h            ← UDP 收包 + LZ4 解压
│   ├── TrtInference.h           ← TRT 推理 (引擎加载 / 热切换 / CUDA stream / BGRA buffer)
│   ├── CudaPreprocess.h         ← GPU 预处理 (NVRTC 运行时编译)
│   └── UdpReplySender.h         ← 检测结果回传主机
│
├── src/
│   ├── main.cpp                 ← 主循环: 双线程 + LIFO + 绑核 + 统计
│   ├── UdpReceiver.cpp          ← recvfrom 非阻塞排空 + ProcessDatagram + LZ4_decompress_safe
│   ├── TrtInference.cpp         ← deserializeCudaEngine + Infer (GPU 流水线)
│   ├── CudaPreprocess.cpp       ← NVRTC 编译 CUDA kernel + cuLaunchKernel
│   └── UdpReplySender.cpp       ← ReplyHeader + DetectionRaw[] 打包 → sendto
│
└── test/                        ← 独立推理测试 (不链接 client 主程序)
    ├── CMakeLists.txt            ← 独立构建，引用 ../src/ 中的 TrtInference/CudaPreprocess
    ├── include/ImageUtils.h      ← WIC 图片加载 + 画框
    ├── src/main.cpp              ← test_infer.exe: 加载图片 → 推理 → 输出 txt + BMP
    ├── image/                    ← 测试图片 (delta.jpg, bf6.jpg, apex.jpg, ow2.jpg)
    └── result/                   ← 测试输出

shared/include/
├── PacketHeader.h               ← 24B Host→Client 协议 (width/height 动态 ROI + modelId 热切换)
└── ReplyPacket.h                ← 16B Client→Host 协议 (DetectionRaw[])
```

---

## 4. 数据流详解

### 4.1 网络层: ReassemblyBuffer + UdpReceiver

```
UDP datagram (max 1420B = 20B header + 1400B payload)
    │
    ▼ ProcessDatagram()
    │  1. magic != 0x5358 → drop
    │  2. frameId > expected → discard old frame, start new (铁律: 不等残缺帧)
    │  3. frameId < expected → drop (stale)
    │  4. chunkIndex × 1400 → memcpy 到 data[] 正确偏移
    │  5. receivedMask 去重
    │  6. chunksReceived == totalChunks → 完整 → LZ4_decompress_safe
    │
    ▼ 输出: W×H×4 BGRA (动态尺寸，来自 PacketHeader.width/height)
```

关键设计:
- `recvfrom()` 非阻塞 (`FIONBIO`)，一次性排空所有排队数据报
- 预分配 67 MB 缓冲区 (4096² 最坏情况)，稳态零分配
- Frame ID 用 `int32_t(a - b) > 0` 安全处理 uint32 回绕 (170Hz 下 ~290 天)

### 4.2 推理层: TrtInference + CudaPreprocess

```
Infer(cpu_bgra):
  ┌─ cudaMemcpyAsync(m_dBgraInput, cpu_bgra, 1.66MB, H→D, stream)
  ├─ LaunchBgra8ToFp32ChwRgb(m_dBgraInput, m_dInput, 416, 416, stream)
  │    └─ GPU kernel (NVRTC): 每线程一像素
  │       BGRA uint8 → /255.0f → B↔R swap → CHW planar FP32
  ├─ enqueueV3(stream)           ← TRT FP16 推理
  ├─ cudaStreamSynchronize(stream)  ← 唯一 CPU 阻塞点
  └─ cudaMemcpy(output, D→H)     ← 300×6 floats (7.2 KB)
  → postprocess: threshold + clamp
```

关键设计:
- 整条 GPU 流水线在**同一个非默认 CUDA stream** 上排队
- CPU 只在最后 `cudaStreamSynchronize` 一次
- BGRA→FP32 kernel 启动开销 ~5μs，执行 ~15-30μs

### 4.3 回复层: UdpReplySender

```
ReplyHeader (16B): magic(0x5359) frameId(4) numDets(2) padding(8)
DetectionRaw[] (N×24B): x1,y1,x2,y2,conf(f32), classId(u32)
→ sendto(host:8889), 非阻塞，fire-and-forget
```

坐标在模型像素空间 `[0,415]×[0,415]`，主机自行加 ROI 偏移量映射。

### 4.4 Engine 热切换 (v4)

```
主机切换游戏 → PacketHeader.modelId 变化
    │
    ▼ Producer (UdpReceiver::ProcessDatagram)
    │  g_targetModelId.store(header->modelId)  每次合法新帧都更新
    │
    ▼ Consumer (TrtInference::Infer)
    │  if (m_currentModelId != g_targetModelId):
    │    1. cudaStreamSynchronize(stream)      ← 等上一帧 GPU 任务完成
    │    2. UnloadEngine()                     ← 销毁 context → engine → GPU buffers
    │    3. LoadEngineFile(GetModelPath(id))   ← 映射 ID→路径, 反序列化, 分配 IO
    │    4. m_currentModelId = id
    │    5. return {}                          ← 丢弃当前帧 (旧图 ≠ 新模型)
    │
    ▼ 下一帧: 正常 GPU pipeline 用新 engine
```

模型 ID 映射 (TrtInference::GetModelPath):
| modelId | 引擎 | 类别 |
|---------|------|------|
| 0 | apex_enemy_416.engine | enemy (1类) |
| 1 | delta_body_head_416.engine | body + head (2类) |
| 0 | bf6_enemy_self_416.engine | enemy + self (2类) |
| 3 | ow2_enemy_416.engine | enemy (1类) |

关键安全保证:
- Context 必须在 Engine 之前销毁（TRT API 要求）
- `cudaStreamSynchronize` 确保旧 engine 的进行中推理完全结束
- Runtime + CUDA stream 保持不变，仅重建 engine/context/buffers
- 切换帧返回空检测，Consumer 立即取下一帧新图喂新模型

### 4.5 主循环: main.cpp

```
main():
  解析参数 → 初始化各模块
  ├─ 主线程 = Producer (core 0): 收包 → LIFO push → 每秒统计
  └─ consumer 线程 (core 1): cudaSetDevice → SetupStream
       → InitCudaPreprocess (NVRTC 编译 kernel)
       → Warmup 50 帧黑图 → while: LIFO pop → Infer → reply

FrameSlot (LIFO size-1):
  mutex + condition_variable
  Producer: lock → data = move(newFrame) → hasNew=true → notify
  Consumer: wait_for(2ms) → if hasNew: move(data) → unlock → Infer
```

---

## 5. 线程模型

| 线程 | 核心 | 职责 | 阻塞点 |
|------|------|------|--------|
| Producer (main) | 0 | UDP 收包、LZ4 解压、LIFO push | `recvfrom` (非阻塞，WSAEWOULDBLOCK 即返回) |
| Consumer (worker) | 1 | GPU 推理、回复发送 | `cudaStreamSynchronize` (等待 GPU 完成) |

- `SetThreadAffinityMask` 绑核，物理隔离 L3 cache
- `std::atomic` 跨线程共享统计计数器
- `g_running` + `cv.notify_all()` 优雅关闭

---

## 6. 构建系统

### 6.1 主构建 (client/)

```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
# 输出: build_x64/RelWithDebInfo/SynapseX_Client.exe
```

依赖链:
```
SynapseX_Client.exe
  ├── SynapseX_Inference.lib (TrtInference.cpp + CudaPreprocess.cpp)
  │     ├── nvinfer_10.lib + nvinfer_plugin_10.lib (TensorRT 10.16)
  │     ├── cudart.lib + cuda.lib + nvrtc.lib (CUDA 13.1)
  ├── lz4_static.lib (thirdparty/lz4-1.10.0)
  ├── ws2_32 (Winsock2)
  └── SynapseX_Shared (header-only, shared/include/)
```

### 6.2 测试构建 (test/)

完全独立，不污染主构建:
```powershell
cd client/test
cmake -S . -B build_x64 -G "Visual Studio 18 2026" -A x64
cmake --build build_x64 --config RelWithDebInfo
# 输出: build_x64/RelWithDebInfo/test_infer.exe
```

链接 `../src/TrtInference.cpp` + `../src/CudaPreprocess.cpp` (相对路径引用)。

---

## 7. 环境依赖与踩坑记录

### 7.1 软件版本矩阵

| 组件 | 版本 | 路径 | 备注 |
|------|------|------|------|
| Windows | 11 Enterprise 10.0.26200 | — | |
| Visual Studio | 2026 (v18) | `C:\Program Files\Microsoft Visual Studio\18\` | **CUDA 13.1 不官方支持 VS 2026** |
| MSVC toolchain | 14.51 (v180) | 同上 | 编译主 C++ 代码 |
| MSVC toolchain | 14.44 (v143) | 同上 `.../MSVC/14.44.35207/` | **NVRTC 运行时编译 kernel 所需** |
| CUDA Toolkit | 13.1.115 | `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1` | |
| TensorRT | 10.16.1.11 | `C:\Program Files\NVIDIA\TensorRT-10.16.1.11...\TensorRT-10.16.1.11` | 注意路径有两层 |
| CMake | 4.4 / 3.28+ | `C:\Program Files\CMake\bin` | |
| GPU | RTX 4060 Laptop (SM 8.9) | — | laptop 性能有限，推理 5-6ms |

### 7.2 坑 #1: nvcc 与 VS 2026 不兼容

**症状**: `nvcc fatal: Unsupported Microsoft Visual Studio version`  
**尝试**: `-allow-unsupported-compiler` 绕过了版本检查，但 `cudafe++` 在编译时 `ACCESS_VIOLATION` 崩溃  
**根因**: CUDA 13.1 最多支持 VS 2022 (v143)，VS 2026 (v180) 的 MSVC 二进制接口不兼容  
**解决**: 放弃构建期 nvcc，改用 **NVRTC 运行时编译** CUDA kernel

### 7.3 坑 #2: CMake `enable_language(CUDA)` 失败

**症状**: `No CUDA toolset found`  
**根因**: VS 2026 缺少 CUDA MSBuild 集成 (`CUDA 13.1.props` 未注册)  
**解决**: 同 #1，NVRTC 方案不需要 CMake 的 CUDA 语言支持

### 7.4 坑 #3: NVRTC 运行时编译需要 v143 MSVC

**症状**: `nvrtcCompileProgram FAILED`  
**根因**: NVRTC 在运行时用主机 C++ 编译器编译 GPU kernel，CUDA 13.1 的 NVRTC 只支持 VS 2019-2022 (v142/v143)  
**解决**: VS Installer → 单个组件 → 安装 **"MSVC v143 - VS 2022 C++ x64/x86 build tools"**  
**验证**: `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207\` 存在即可

### 7.5 坑 #4: 运行时 DLL 缺失

**症状**: `nvinfer_10.dll not found` / `cudart64_131.dll not found`  
**解决**: 启动前必须设置 PATH:
```powershell
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"
```

### 7.6 坑 #5: TRT Engine 绑定 GPU

**症状**: `deserializeCudaEngine FAILED` 或推理结果全错  
**根因**: `.engine` 文件绑定 GPU 型号 + CUDA/TensorRT 版本。不同机器/不同 GPU 不能共用  
**解决**: 每台部署机器必须用 `trtexec` 从 ONNX 重新生成:
```powershell
trtexec --onnx=model.onnx --saveEngine=model.engine --fp16
```
ONNX 是通用格式，可以跨机器共享。

### 7.7 坑 #6: 控制台编码乱码

**症状**: 启动日志出现 `鈥?` 等乱码  
**根因**: 源代码中的 `—` (em-dash, U+2014) 在 GBK 控制台下无法渲染  
**解决**: 所有 `fprintf` 字符串改用 `--` 替代 `—`

### 7.8 坑 #7: LIBCMT 链接警告

**症状**: `LNK4098: 默认库"LIBCMT"与其他库的使用冲突`  
**根因**: NVRTC 静态库用 `/MT` (static CRT)，项目用 `/MD` (dynamic CRT)  
**影响**: 无害，可忽略

---

## 8. 协议速查

### Host → Client (UDP :8888)
```
PacketHeader 24B (packed):
  offset  size  field
  0       2     magic = 0x5358
  2       4     frameId (monotonic)
  6       2     totalChunks
  8       2     chunkIndex
  10      4     totalSize (compressed bytes)
  14      2     payloadSize (this packet)
  16      2     width  (ROI)
  18      2     height (ROI)
  20      1     modelId (0-3, triggers engine hot-swap)
  21      3     padding (reserved)
  + LZ4 payload (≤1400B)
```

### Client → Host (UDP :8889)
```
ReplyHeader 16B: magic(0x5359) frameId(4) numDets(2) padding(8)
DetectionRaw 24B: x1 y1 x2 y2(f32) conf(f32) classId(u32)
```

---

## 9. 性能概览

| 指标 | 值 | 测量条件 |
|------|-----|----------|
| 网络 + 重组 + 解压 | 0.10–0.25 ms | Producer core 0 |
| H→D copy (BGRA) | ~0.02 ms | PCIe Gen4 |
| GPU kernel (BGRA→FP32) | ~0.02 ms | 416×416, memory-bound |
| TRT enqueueV3 | ~1.5 ms (5070Ti) / ~5.2 ms (4060 Laptop) | FP16 |
| Postprocess + reply | ~0.05 ms | |
| 全链路 | ~1.7 ms (5070Ti) / ~5.5 ms (4060 Laptop) | 单帧端到端 |
| 170 FPS 预算 | 5.88 ms | 5070Ti 余量 70%+ |

---

## 10. 快速上手 Checklist

新机器部署按此顺序:
1. 装 VS 2026，确保勾选 **v143 MSVC 工具链** (§7.4)
2. 装 CUDA 13.1 + TensorRT 10.16
3. 确认路径匹配 `CMakeLists.txt` 中的 `TRT_BASE_PATH` 和 CUDA 路径
4. 设置 DLL PATH (§7.5)
5. `cd client && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo`
6. 构建测试: `cd client/test && cmake -S . -B build_x64 -G "Visual Studio 18 2026" -A x64 && cmake --build build_x64 --config RelWithDebInfo`
7. 用 `trtexec` 从 ONNX 生成 engine (§7.6)
8. 启动: `.\SynapseX_Client.exe [port] [engine] [hostIp] [--save]`
9. 验证: 看到 `[CudaPreprocess] Ready.` 和 `[TrtInference] Ready. (GPU preprocess via NVRTC)` 即成功
10. 跑测试: `cd test && .\build_x64\RelWithDebInfo\test_infer.exe ..\model\engine\<xxx>.engine .\image\<xxx>.jpg`

---

## 11. 术语表

| 术语 | 含义 |
|------|------|
| Producer | 网络收包线程 (core 0)，UDP+LZ4 解压 → LIFO 推入 |
| Consumer | GPU 推理线程 (core 1)，LIFO 取出 → Infer → reply |
| LIFO | Last-In-First-Out 队列，size=1，永远只留最新帧 |
| NVRTC | NVIDIA Runtime Compilation — 运行时编译 CUDA kernel，不需要 nvcc |
| FrameSlot | LIFO 队列的数据结构 (mutex + cv + frame data) |
| Iron Law | 新 frameId 到达 → 立即丢弃旧残缺帧，绝不等待 |
| TRT | TensorRT |
| ROI | Region of Interest，主机截屏的中心裁剪区域 |
| ONNX | Open Neural Network Exchange — 通用模型格式，GPU 无关 |
| Engine (.engine) | TRT 序列化的优化模型，GPU 绑定 |
| WIC | Windows Imaging Component — 图片解码 (JPEG/PNG/BMP) |
