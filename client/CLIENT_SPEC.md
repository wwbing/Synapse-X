# Synapse-X 副机端规格说明

> 最后更新: 2026-07-05 | 基于提交 `9cd7839` | 以代码为权威来源

## 1. 流水线概览

```
Host → UDP :8888 (24B PacketHeader + LZ4 chunks)
         │
    ┌────┴──── Producer Thread (core 0) ────────────────────────────┐
    │  [1] UdpReceiver      非阻塞排空 + 乱序重组 + LZ4 解压       │
    │      拼好一帧 → LIFO push（覆盖旧帧，唤醒 Consumer）          │
    │      g_targetModelId 更新（来自 PacketHeader.modelId）        │
    └───────────────────────┬───────────────────────────────────────┘
                            │  FrameSlot (size=1, mutex+cv)
    ┌───────────────────────┴───────────────────────────────────────┐
    │  [2] Consumer Thread (core 1)                                 │
    │      cudaMemcpyAsync(BGRA H→D)                                │
    │      → GPU kernel (NVRTC): BGRA→FP32 CHW RGB                  │
    │      → enqueueV3(stream): TRT FP16                             │
    │      → cudaStreamSynchronize                                  │
    │      → 后处理 (confThr=0.25, 300 dets)                        │
    │      → UdpReplySender                                         │
    │      [每帧检查 g_targetModelId → 热切换引擎]                   │
    └───────────────────────┬───────────────────────────────────────┘
                            │
Host ← UDP :8889 (16B ReplyHeader + DetectionRaw[])
```

**全链路 ~1.5–1.8 ms**，170 FPS 预算 5.88 ms，余量 > 70%。

---

## 2. 架构细节

### 2.1 异步生产者-消费者模式

```
Producer (main thread, core 0):       Consumer (worker, core 1):
  PinToCore(0)                          PinToCore(1)
  while g_running:                      cudaSetDevice(0)
    TryReceive() → gotFrame?            SetupStream()
      lock → push to FrameSlot          InitCudaPreprocess()  ← NVRTC 运行时编译
      cv.notify_one()                   LoadEngine(g_targetModelId)
  Per-second stats + BMP                预热 50 帧黑图
                                        cudaDeviceSynchronize
                                        while g_running:
                                          cv.wait_for(2ms) → pop FrameSlot
                                          热切换检查 (g_targetModelId)
                                          Infer(frame) → SendReplies
```

- **FrameSlot**: `mutex` + `condition_variable`，size=1。生产者覆盖旧帧时 `drops++`
- **消费者超时**: `cv.wait_for(2ms)`，无新帧时周期性唤醒检查 `g_running`
- **绑核**: Producer=core 0, Consumer=core 1
- **LIFO 语义**: 消费者始终获取最新帧，丢弃过期帧防止队头阻塞

### 2.2 引擎热切换

Producer 从 `PacketHeader.modelId` 写入 `g_targetModelId`（`std::atomic<uint8_t>`）。
Consumer 在每帧 `Infer()` 开始时检查：

```
if (g_targetModelId != m_currentModelId):
    1. cudaStreamSynchronize  — 确保前一帧 GPU 工作完成
    2. UnloadEngine()         — 销毁旧上下文 + 引擎 + GPU 缓冲区
    3. LoadEngine(targetId)   — 加载新引擎文件
    4. return empty           — 丢弃过时帧（旧图像 ≠ 新模型）
```

模型映射（`TrtInference::GetModelPath()`）：

| modelId | 游戏 | 引擎文件路径 |
|---------|------|-------------|
| 0 | Apex Legends(新) | `../../model/engine/apex_enemy_self_416.engine` |
| 1 | Delta Force | `../../model/engine/delta_body_head_416.engine` |
| 2 | Battlefield 6 | `../../model/engine/bf6_enemy_self_new.engine` |
| 3 | Overwatch 2 | `../../model/engine/ow2_enemy_416.engine` |
| 4 | Aimlabs | `../../model/engine/aimlabs_enemy_416.engine` |
| 5 | PUBG | `../../model/engine/pubg_body_head_416.engine` |
| 6 | CrossFire | `../../model/engine/cf_body_head_416.engine` |
| 7 | CS2 | `../../model/engine/cs2_enemy_self_416.engine` |

### 2.3 NVRTC GPU 预处理

**文件:** `src/CudaPreprocess.cpp`

由于 CUDA 13.1 的 `cudafe++` 与 VS 2026 (v180) MSVC 不兼容，使用 NVRTC 运行时编译完全避开构建期 nvcc：

```
InitCudaPreprocess():                     // Consumer 线程 init 时调用一次
  nvrtcCreateProgram(kernel_source)       // 内核源码在 C++ 字符串中
  nvrtcCompileProgram(                    // --gpu-architecture=compute_86
      --gpu-architecture=compute_86       //   -std=c++17
      -std=c++17                          //   -use_fast_math
      -use_fast_math                      //   -restrict
      -restrict)
  nvrtcGetPTX() → cuModuleLoadData()     // 加载 PTX
  cuModuleGetFunction()                   // 缓存 CUfunction 句柄

LaunchBgra8ToFp32ChwRgb(d_bgra, d_fp32, w, h, stream):
  cuLaunchKernel(func, grid, block, 0, stream, args, 0)
  // 16x16 线程, ceil(w/16) x ceil(h/16) 块
```

**内核功能：** 每线程一个像素 — BGRA uint8 → `/255.0f` 归一化 → B↔R 通道交换 → CHW planar FP32。

### 2.4 CUDA 流流水线

整个 GPU 操作在同一个非默认 CUDA stream 上排队：

```
cudaMemcpyAsync(BGRA, H→D, stream)
→ LaunchBgra8ToFp32ChwRgb(..., stream)    [GPU kernel]
→ enqueueV3(stream)                       [TRT inference]
→ cudaStreamSynchronize(stream)           [唯一阻塞点]
→ cudaMemcpy(output, D→H)                [读取结果到主机]
```

### 2.5 推理预热

Consumer 线程 init 后跑 50 帧黑图推理：
- 拉升 GPU P-State 到最高频率
- 完成 TRT 引擎的所有 JIT 编译
- `cudaDeviceSynchronize` 确保预热完成后才进入主循环

### 2.6 乱序重组 (ReassemblyBuffer)

**文件:** `include/ReassemblyBuffer.h` (header-only)

- 数据块按 `chunkIndex * MAX_PAYLOAD_SIZE` 偏移放置
- `receivedMask` 位掩码检测重复数据块
- 更新的 `frameId` → 旧未完成帧立即丢弃
- 过期数据包（frameId 落后）静默丢弃
- 预分配到最坏情况 (4096x4096 ROI): ~67 MB
- `IsNewerFrameId()` 使用 `int32_t` 差值处理 uint32 环绕

### 2.7 帧组装超时

12ms 超时（≈ 2x 170Hz 周期）。部分帧在超时后丢弃，防止主机中途停止发送时挂起。

---

## 3. 环境与依赖

| 组件 | 版本 | 说明 |
|------|------|------|
| Visual Studio | 2026 (v18) | 需要 v143 MSVC 工具链（NVRTC 主机编译器） |
| CUDA Toolkit | 13.1 / 13.2 | `find_package(CUDAToolkit)` |
| TensorRT | 10.16.1.11 | 硬编码路径: `C:/Program Files/NVIDIA/TensorRT-10.16.1.11...` |
| CMake | ≥ 3.28 | |
| Windows SDK | 10.0.26100 | |

**运行时 DLL PATH:**
```powershell
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"
```

---

## 4. 构建与运行

```powershell
cd client
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo

# 运行
.\build_x64\RelWithDebInfo\SynapseX_Client.exe [port=8888] [engine=../../model/bf416.engine] [hostIp=192.168.100.1] [--save]
```

`--save` 标志每秒保存一张 BMP (`client_0000.bmp`) 用于调试。

---

## 5. 统计输出

```
[Client] Stats: roi=416x416 fps=170.0 frames=170 dropped=0 drop_rate=0.0%
         lifo_drops=0 recv_ms=0.12 infer_ms=1.58 total_ms=1.70
         infer_per_sec=170 bandwidth_mb_s=3.45
```

| 字段 | 含义 |
|------|------|
| `recv_ms` | UDP 收包 + 重组 + LZ4 解压（Producer） |
| `infer_ms` | cudaMemcpyAsync + GPU kernel + TRT + sync + 后处理（Consumer） |
| `total_ms` | recv + infer，全链路单帧 |
| `lifo_drops` | GPU 跟不上被覆盖的帧数 |
| `drop_rate` | 网络丢包率 |

---

## 6. 文件结构

```
client/
├── CLIENT_SPEC.md               ← 本文件
├── CMakeLists.txt               LZ4 + CUDA + TRT + NVRTC + ws2_32
├── CMakePresets.json            VS 2026 x64
├── model/
│   ├── *.onnx                   通用 ONNX 模型 (GPU 无关)
│   └── engine/                  编译后 .engine 文件 (GPU/TRT 版本绑定)
├── include/
│   ├── ReassemblyBuffer.h       乱序重组引擎 (header-only)
│   ├── UdpReceiver.h            收包 + 解压
│   ├── TrtInference.h           TRT 推理 (热切换 + CUDA stream)
│   ├── CudaPreprocess.h         GPU kernel 声明 (NVRTC)
│   └── UdpReplySender.h         回复通道
├── src/
│   ├── UdpReceiver.cpp          收包 / 重组 / 解压
│   ├── TrtInference.cpp         TRT 引擎加载 + Infer (GPU pipeline + 热切换)
│   ├── CudaPreprocess.cpp       NVRTC 编译 + Driver API 启动
│   ├── UdpReplySender.cpp       打包回复 + sendto
│   └── main.cpp                 双线程主循环 + LIFO + 绑核 + 统计
└── test/
    ├── CMakeLists.txt            独立推理测试
    ├── include/ImageUtils.h      图片加载工具
    ├── src/main.cpp              推理验证 (CUDA + TRT + WIC + COM)
    ├── image/                    测试图片 (*.jpg)
    └── result/                   预期结果 (*_detections.txt, *_boxes.bmp)
```

---

## 7. 重新生成 TRT 引擎

`.engine` 绑定 GPU 型号 + CUDA/TensorRT 版本。换机器或升级驱动后必须重建：

```powershell
& "C:\Program Files\NVIDIA\TensorRT-10.16.1.11.Windows.amd64.cuda-13.2\TensorRT-10.16.1.11\bin\trtexec.exe" `
    --onnx=client\model\xxx.onnx `
    --saveEngine=client\model\engine\xxx.engine `
    --fp16
```

---

## 8. 故障排除

| 症状 | 原因 | 解决方法 |
|------|------|------|
| `nvinfer_10.dll` 找不到 | TensorRT bin 不在 PATH | 添加 TRT/bin 到 PATH |
| `cudart64_131.dll` 找不到 | CUDA bin 不在 PATH | 添加 CUDA/v13.1/bin 到 PATH |
| NVRTC 编译失败 | 缺少 v143 MSVC | 安装 VS 2022 build tools (v143) |
| `deserializeCudaEngine FAILED` | engine 文件损坏或 GPU 不匹配 | 重建 engine |
| 推理尖刺 3→27ms | GPU P-State 波动 | `nvidia-smi -lgc 2500` 锁频率 |
| 热切换失败 | 对应 .engine 文件不存在 | 检查 `model/engine/` 目录 |
| 丢包率 > 0% | 网络丢包或 socket buffer 不够 | 检查网线 / 加大 SO_RCVBUF |
| LNK4098 警告 | /MT vs /MD CRT 冲突 | 无害，忽略 |

---

## 9. 已知问题

| # | 问题 | 严重度 | 说明 |
|----|------|--------|------|
| I1 | 推理延迟偶发尖刺 | 中 | GPU P-State 波动或 driver DPC。锁 GPU 频率可缓解 |
| I2 | 解压缓冲区 67 MB 预分配 | 低 | 为 4096^2 分配，416^2 只用 ~700 KB |
| I3 | 回复通道无重试 | 低 | UDP fire-and-forget |
| I4 | 坐标无帧间平滑 | 中 | 可加 IoU 匹配 + EMA |
| I5 | VS 2026 + nvcc 不兼容 | 已规避 | NVRTC 方案 |

---

## 10. 快速参考

```
构建:   cd client && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
运行:   .\build_x64\RelWithDebInfo\SynapseX_Client.exe [port] [engine] [hostIp] [--save]
DLL:    TRT\bin + CUDA\v13.1\bin 必须在 PATH
架构:   Producer(core0) → LIFO(1) → Consumer(core1)
预处理: GPU via NVRTC (运行时编译，无需 nvcc)
输入:   UDP :8888 (24B PacketHeader + LZ4 chunks)
回复:   UDP :8889 → Host (16B ReplyHeader + DetectionRaw[])
模型:   6 款游戏，416x416 FP16，运行时热切换
延迟:   ~1.5–1.8 ms
FPS:    170 稳态，0% 丢帧
```
