# Synapse-X Client Specification

> **Status**: 已交付 v2 — 异步双线程架构。170 FPS 稳态，LIFO 零积压。
> **副机 IP**: 192.168.100.2 | **主机 IP**: 192.168.100.1

---

## 1. Pipeline Overview (v2 — Async)

```
Host → UDP :8888 (20B PacketHeader + LZ4 chunks)
         │
    ┌────┴──── Producer Thread (core 0) ──────────────────────┐
    │  [1] UdpReceiver      0.10–0.25 ms                      │
    │      非阻塞排空 + 乱序重组 + LZ4 解压                    │
    │      拼好一帧 → LIFO push（覆盖旧帧，唤醒 Consumer）     │
    └───────────────────────┬─────────────────────────────────┘
                            │  FrameSlot (size=1, mutex+cv)
    ┌───────────────────────┴─────────────────────────────────┐
    │  [2] Consumer Thread (core 1, affinity pinned)          │
    │      TrtInference     1.50–2.50 ms                      │
    │      BGRA→FP32 CHW → GPU(FP16, dedicated stream) → det  │
    │      UdpReplySender   < 0.01 ms                         │
    │      fire-and-forget UDP → Host:8889                    │
    └───────────────────────┬─────────────────────────────────┘
                            │
Host ← UDP :8889 (16B ReplyHeader + DetectionRaw[])
```

**全链路 ~1.6–2.8 ms**（推理预热后），170 FPS 预算 5.88 ms 内，余量 > 50%。
Producer **永不等待 GPU** — 推理积压仅触发 LIFO 覆盖，不影响收包。

---

## 2. Architecture: Async Producer-Consumer

### 2.1 LIFO Drop Queue (FrameSlot)

```
FrameSlot {
    mutex, condition_variable
    data, frameId, roiW, roiH
    hasNew : bool
    drops  : uint64   // 帧被覆盖次数（Consumer 没来得及取）
}

Producer:                          Consumer:
  lock(mtx)                          lock(mtx)
  if hasNew: drops++                 wait(cv, 2ms timeout) until hasNew || !g_running
  data = move(newFrame)              if hasNew:
  hasNew = true                        frame = move(data)
  unlock(mtx)                          hasNew = false
  cv.notify_one()                      hasNewDrops = drops  // 读走覆盖计数
                                     unlock(mtx)
                                     Infer(frame) → reply
```

关键性质：
- **size=1** — 永远只留最新帧，彻底消灭积压
- **Producer 永不等** — `lock_guard` 只在 push 时持锁，毫秒级
- **Consumer 有超时** — `wait_for(2ms)`，即使无新帧也周期性唤醒检查 `g_running`

### 2.2 Core Affinity

```cpp
Producer (main thread):  PinThreadToCore(0)  // 网络收包独享核心
Consumer (worker):       PinThreadToCore(1)  // GPU 推理独享核心
```

物理隔离避免 L3 cache 竞争和线程迁移导致的延迟毛刺。

### 2.3 CUDA Stream & Warmup

- Consumer 线程启动后：`cudaSetDevice(0)` → `cudaStreamCreateWithFlags(cudaStreamNonBlocking)`
- `enqueueV3(dedicated_stream)` — 独立 CUDA stream，消除 default stream 的隐式同步
- **50 帧黑图预热**：逼出 GPU max P-State + 完成驱动 JIT 编译
- 预热后 `cudaDeviceSynchronize` 确保稳态

### 2.4 Frame Assembly Timeout

Producer 跟踪上次收包时间。若 `TryReceive` 连续返回 false 超过 12ms（2× 170Hz 帧周期），默认 Host 已停发，跳过等待。

---

## 3. Files

```
client/
├── CMakeLists.txt              LZ4 + CUDA 13.1 + TRT 10.16 + ws2_32
├── CMakePresets.json           VS 2026 x64
├── model/
│   ├── bf416.onnx              通用 ONNX (9.3 MB, GPU 无关)
│   └── bf416.engine            编译后引擎 (7.4 MB, GPU/TRT 绑定)
├── include/
│   ├── ReassemblyBuffer.h      乱序重组引擎 (header-only struct)
│   ├── UdpReceiver.h           收包 + 解压
│   ├── TrtInference.h          TRT 推理封装 (含 CUDA stream)
│   └── UdpReplySender.h        回复通道
└── src/
    ├── UdpReceiver.cpp
    ├── TrtInference.cpp
    ├── UdpReplySender.cpp
    └── main.cpp                异步双线程主循环 + LIFO + 绑核 + 统计

shared/include/
├── PacketHeader.h              20B Host→Client 协议
└── ReplyPacket.h               16B Client→Host 协议
```

---

## 4. Build & Run

```powershell
# DLL PATH
set "PATH=C:\Program Files\NVIDIA\TensorRT-10.16.1.11...\bin;%PATH%"
set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\bin;%PATH%"

# 默认启动
.\SynapseX_Client.exe

# 参数: [port=8888] [engine=../../model/bf416.engine] [hostIp=192.168.100.1] [--save]
```

---

## 5. Protocol Summary

### Host → Client (data, port 8888)
```
PacketHeader 20B: magic(0x5358) frameId(4) totalChunks(2) chunkIdx(2)
                  totalSize(4) payloadSize(2) width(2) height(2)
                  + LZ4 payload ≤1400B
```

### Client → Host (reply, port 8889)
```
ReplyHeader 16B: magic(0x5359) frameId(4) numDets(2) padding(8)
                 + DetectionRaw[]. N×24B:
                   x1 y1 x2 y2(f32) conf(f32) classId(u32)
```
坐标在 416×416 模型像素空间。Host 用 `roiOffset + det` 映射到屏幕。

---

## 6. Stats Output

```
---- per-second stats --------------------------------
  ROI: 416x416  |  FPS:   170.0  |  fr:   170  |  drop:     0 ( 0.0%)  |  LIFO drops: 0
  recv:   0.12 ms  |  infer:   1.85 ms  |  total:   1.97 ms  |  infer/s:   170  |  BW:   3.45 MB/s
```

| Field | Meaning |
|-------|---------|
| `ROI` | 当前帧尺寸 |
| `FPS` | Producer 每秒完整帧数 |
| `fr` / `drop` | 成功帧 / 丢弃帧（含网络丢包 + LIFO 覆盖） |
| `LIFO drops` | Consumer 跟不上被覆盖的帧数（0 = 推理跟得上） |
| `recv` | Producer 侧耗时 (UDP + 重组 + 解压) |
| `infer` | Consumer 侧耗时 (预处理 + GPU + 后处理) |
| `total` | recv + infer |
| `infer/s` | 每秒推理数 |

每 30 推理帧打印检测结果 (top 3)。

---

## 7. Key Performance

| 指标 | 稳态 | 高动态 |
|------|------|--------|
| 收包+重组+解压 | 0.10–0.25 ms | 0.10–0.25 ms |
| 推理 (pre+GPU+post) | 1.50–2.50 ms | 1.50–2.50 ms |
| 全链路总延迟 | 1.6–2.8 ms | 1.6–2.8 ms |
| 丢帧率 | 0% | ~0% (< 1%) |
| LIFO 覆盖 | 0 | 低 (GPU 跟得上) |
| 网络吞吐 | ~3.4 MB/s | ~3.4 MB/s |

---

## 8. 交付清单

| 模块 | 状态 |
|------|------|
| UDP 非阻塞收包 + 乱序重组引擎 | 完成 |
| LZ4 raw block 解压 + 动态 ROI 校验 | 完成 |
| TensorRT FP16 推理 (bf416.engine) | 完成 |
| BGRA→FP32 CHW RGB 预处理 | 完成 |
| **异步双线程架构 (Producer-Consumer)** | **完成** |
| **LIFO size-1 覆盖队列 (mutex+cv)** | **完成** |
| **绑核策略 (Producer=core0, Consumer=core1)** | **完成** |
| **专属 CUDA stream (non-blocking)** | **完成** |
| **50 帧黑图预热 + GPU P-State 拉升** | **完成** |
| **帧组装 12ms 超时丢弃** | **完成** |
| Client→Host 检测回复通道 (UDP 8889) | 完成 |
| 每秒统计 (FPS / 丢帧 / LIFO覆盖 / 延迟) | 完成 |
| `--save` 参数控制每秒 BMP 存图 | 完成 |
| CUDA 13.1 + TRT 10.16 CMake 自动寻址 | 完成 |

---

## 9. 已知问题

| # | 问题 | 严重度 | 方向 |
|----|------|--------|------|
| I1 | CPU 预处理瓶颈 | 中 | BGRA→FP32 双重循环 `/255.0f` 占 ~2ms。用 CUDA kernel 或 SIMD 可降 |
| I2 | 推理延迟偶发尖刺 (3→27ms) | 中 | GPU P-State 波动或 driver DPC。锁定 GPU 频率 `nvidia-smi -lgc` 可缓解 |
| I3 | 解压缓冲区 67 MB 预分配 | 低 | 416² 只用 ~700 KB，按实际 ROI 动态调整 |
| I4 | 回复通道无重试 | 低 | UDP fire-and-forget，丢包则 Host 漏收一次检测 |
| I5 | 坐标无帧间平滑 | 中 | 检测框抖动，加 IoU 匹配 + EMA |

---

## 10. 未来优化

- **预处理 GPU 化** — CUDA kernel 做 BGRA→FP32 CHW，省 CPU 2ms，全链路压到 ~1ms
- **CUDA Graph** — 固定 pre+infer+post 执行图，降低 launch 开销
- **锁 GPU 频率** — `nvidia-smi -lgc 2500` 消除 P-State 尖刺
- **坐标平滑** — 帧间 IoU 跟踪 + 指数移动平均
- **多模型/多分辨率** — 运行时切换 engine (416/640/...)
- **按需预分配** — 根据实际 ROI 而非 4096² 分配缓冲区

---

## 11. 速查

```
BUILD:   cd client && cmake --preset windows-x64 && cmake --build build_x64 --config RelWithDebInfo
RUN:     .\build_x64\RelWithDebInfo\SynapseX_Client.exe [port] [engine] [hostIp] [--save]
ARCH:    Producer(core0) → LIFO(1) → Consumer(core1)
DATA:    UDP :8888 ← Host (20B header, LZ4 chunks)
REPLY:   UDP :8889 → Host (16B header, DetectionRaw[])
MODEL:   416×416 FP16, bf416.engine
STREAM:  Dedicated CUDA stream (non-blocking)
WARMUP:  50 black frames
DELAY:   ~1.6–2.8 ms (async), ~3.0–4.5 ms (sync, old)
FPS:     170 sustained, 0% drop
```
