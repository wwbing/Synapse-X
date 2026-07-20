# Synapse-X 主机端规格说明

> 最后更新: 2026-07-05 | 基于提交 `9cd7839` | 以代码为权威来源

## 1. 管线概览

```
┌────────────────── 170 Hz 固定节奏主循环 ──────────────────┐
│                                                            │
│  Tick N (每 5.882ms):                                      │
│    1. 热键检测 (PageUp/PageDown)                            │
│    2. DXGI 采集 → 有新帧? 压缩+缓存 : 复用缓存              │
│    3. UDP 分片发送 (每 tick 都发送)                         │
│    4. UDP 回复接收 (排空所有排队数据报)                     │
│    5. 目标选择 → PD 自瞄                                    │
│    6. sleep_until(nextTick) 维持 170Hz                      │
│                                                            │
└────────────────────────────────────────────────────────────┘
```

**关键设计决策：**

- **每 tick 都发送**：即使没有新帧，也重新发送缓存的压缩帧。确保副机始终有数据可用。
- **采集后立即压缩**：只有 DXGI 返回新帧时才执行 LZ4 压缩；否则复用缓存。
- **非阻塞 UDP**：`sendto()` 在非阻塞套接字上执行。网络堆栈拥塞时丢包，管线继续运行。
- **170Hz 固定节奏**：`sleep_until()` 维持精确节奏。落后于计划时立即重置基准时间。

| 阶段 | 模块 | 典型延迟 |
|------|------|----------|
| 采集 | DxgiCapturer — GPU CopySubresourceRegion（仅中心 ROI），Map→memcpy | ~0.15 ms |
| 压缩 | Lz4Compressor — `LZ4_compress_fast(accel=5)`，预分配缓冲区 | ~0.18 ms |
| 发送 | UdpSender — 非阻塞，4MB 缓冲区，栈分配头部+负载 | ~0.02 ms |
| 接收回复 | UdpReplyReceiver — 非阻塞排空，模型→屏幕坐标映射 | <0.01 ms |
| 瞄准 | MouseController — PD + 亚像素 + 延迟补偿 + 空间锁定 | <0.01 ms |
| **主机端总计** | | **~0.35 ms** |
| 副机推理 | TrtInference — NVRTC GPU 预处理 + TRT FP16 | **~1.5–1.8 ms** |
| **端到端** | | **~2 ms** |

---

## 2. 模块详解

### 2.1 DxgiCapturer — GPU 截屏

**文件：** `src/DxgiCapturer.cpp`, `include/DxgiCapturer.h`

**数据流：**
```
桌面帧 (VRAM)
    │ CopySubresourceRegion (仅 ROI 区域，GPU 端)
    ▼
暂存纹理 (VRAM, USAGE_STAGING, CPU_ACCESS_READ)
    │ Map(READ) → 逐行 memcpy → Unmap
    ▼
std::vector<uint8_t> (系统内存, BGRA 连续排列)
```

**初始化流程：**
1. `D3D11CreateDevice()` — D3D11 设备 + 即时上下文（尝试 Debug 层，失败则回退）
2. `IDXGIDevice → IDXGIAdapter` — 获取 GPU 适配器，记录名称/VendorId
3. `EnumOutputs()` — 扫描最多 16 个输出，选择第一个已连接且有非零桌面区域的
4. `DuplicateOutput()` — 创建桌面复制接口
5. `CreateTexture2D(USAGE_STAGING, CPU_ACCESS_READ)` — 创建 ROI 尺寸暂存纹理

**采集循环：**
1. `AcquireNextFrame(timeout=0)` — 立即返回
2. `CopySubresourceRegion()` — GPU 端 ROI 复制
3. `Flush()` — 确保 GPU 复制完成
4. `ReleaseFrame()` — 释放桌面帧
5. `Map(READ)` → 逐行 `memcpy`（处理 RowPitch）→ `Unmap`

**错误恢复：**
- `DXGI_ERROR_ACCESS_LOST`：释放所有资源，sleep(100ms)，重建全链路。500ms 冷却防止高频重试。
- 冷启动重建：`m_initialized==false` 时每个 tick 尝试重建（受冷却限制）
- `E_ACCESSDENIED`：受 500ms 冷却保护

**诊断：**
- `ProtectedContentMaskedOut` 检测 — DRM/反作弊可能阻止采集
- 全零帧检测 — 连续 10 帧后发出警告（典型原因：独占全屏）
- 适配器名称/输出枚举日志

### 2.2 Lz4Compressor — LZ4 压缩

**文件：** `src/Lz4Compressor.cpp`, `include/Lz4Compressor.h`

- `Initialize(maxInputSize)` — 预分配 `LZ4_compressBound(maxInputSize)` 字节
- `Compress()` — `LZ4_compress_fast(accel=5)`，零堆分配
- `GetMaxOutputSize()` — `LZ4_compressBound(inputSize)` 供调用方预分配

**accel=5 理由：** 牺牲约 5% 压缩率换取约 50% CPU 时间。游戏场景（草地/粒子/噪点）在 accel=1 时过度哈希探查。

### 2.3 UdpSender — UDP 分包发送

**文件：** `src/UdpSender.cpp`, `include/UdpSender.h`

- 套接字：`SOCK_DGRAM`，非阻塞 (`FIONBIO`)，`SO_SNDBUF=4MB`
- 热点路径零堆分配：栈上分配 `PacketHeader + MAX_PAYLOAD_SIZE` 字节
- 分片：`totalChunks = ceil(totalSize / 1400)`，偏移 = `chunkIndex × 1400`
- `WSAEWOULDBLOCK` 时丢弃数据包，每 100 次丢弃记录一次日志

### 2.4 UdpReplyReceiver — UDP 回复接收

**文件：** `src/UdpReplyReceiver.cpp`, `include/UdpReplyReceiver.h`

- 绑定 `0.0.0.0:8889`，非阻塞，64KB 接收缓冲区
- 对齐到 64 字节的栈分配接收缓冲区（2048 字节）
- 每个 tick 排空所有数据报直到 `WSAEWOULDBLOCK`
- 坐标映射：`screenX = roiX + modelX`, `screenY = roiY + modelY`
- `roiX = (screenW - roiW) / 2`, `roiY = (screenH - roiH) / 2`

### 2.5 MouseController — 动态 Kp PD 控制器

**文件：** `src/MouseController.cpp`, `include/MouseController.h`

**DLL 加载：** `LoadLibraryA("ddll64.dll")` → `GetProcAddress("OpenDevice")` → `GetProcAddress("MoveR")`

**AimConfig 默认值：**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| Kp | 0.30 | 基础比例增益（远距离温和追踪） |
| Kd | 0.05 | 微分增益（速度阻尼） |
| aimRange | 200.0 | 距屏幕中心最大触发距离（像素） |
| minConfidence | 0.25 | 全局置信度阈值 |
| deltaHeadConfidence | 0.40 | Delta/PUBG 头部专用置信度阈值 |
| aimPoint | 0 | 0=身体(中心), 1=头部(顶部) |
| headOffset | 0.20 | 头部瞄准点比例 (y1 + h*0.2) |
| nativeW/H | 3840×2160 | 显示器原生分辨率 |
| gameW/H | 3840×2160 | 游戏实际分辨率 |
| modelId | 0 | 当前模型选择器 (0-7) |
| kpMax | 0.75 | 近距离最大 Kp（磁吸效果） |
| kpDecay | 0.05 | 衰减陡度 |
| classFilter | 2 | 类别筛选：0=CT/class0, 1=T/class1, 2=全部（仅CS2） |

**PD 算法（每帧，170Hz）：**

1. **延迟补偿** — 减去飞行中的 MoveR（2 帧环形缓冲区）
2. **微死区** — `pixelError < 1.5px` → 不移动
3. **距离门限** — `pixelError > aimRange` → 不移动
4. **动态 Kp** — `Kp + (kpMax - Kp) * exp(-kpDecay * pixelError)`
5. **D 项** — `Kd * (realError - prevError)`
6. **PD 输出** — `outX = currentKp * realDx + Kd * dErrorX`
7. **亚像素累加器** — 累加浮点输出，仅 `|残差| ≥ 1.0` 时发出整数 MoveR
8. **记录延迟历史** — 存储 (moveX, moveY) 到环形缓冲区

### 2.6 HttpTuner — 网页调参面板

**文件：** `src/HttpTuner.cpp`, `include/HttpTuner.h`

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 从 `web/index.html` 提供 HTML |
| GET | `/api/state` | 返回 JSON: config + stats + target |
| POST | `/api/config` | 更新 AimConfig 字段 |

- 后台线程运行 `httplib::Server`，绑定 `0.0.0.0:9999`
- `TuningState` 受 `std::mutex` 保护
- 手写 JSON 序列化（无第三方库依赖）
- 可调参数：Kp, Kd, aimRange, minConfidence, deltaHeadConfidence, headOffset, kpMax, kpDecay, gameW, gameH, modelId, classFilter, aimPoint, aimEnabled

---

## 3. 通信协议

### PacketHeader（主机→副机，24 字节）

定义于 `shared/include/PacketHeader.h`，魔数 `0x5358`。

| 偏移 | 字段 | 大小 | 说明 |
|------|------|------|------|
| 0 | magic | u16 | `0x5358` |
| 2 | frameId | u32 | 单调递增 |
| 6 | totalChunks | u16 | 分片总数 |
| 8 | chunkIndex | u16 | 0-based 分片索引 |
| 10 | totalSize | u32 | 压缩数据总大小 |
| 14 | payloadSize | u16 | 本包载荷字节数 |
| 16 | width | u16 | ROI 宽度 |
| 18 | height | u16 | ROI 高度 |
| 20 | modelId | u8 | 目标模型 (0-5) |
| 21 | padding[3] | u8 | 保留 |

### ReplyPacket（副机→主机）

定义于 `shared/include/ReplyPacket.h`，魔数 `0x5359`。

ReplyHeader (16B) + DetectionRaw[] (每个 24B)，最多 50 个检测结果。

---

## 4. 命令行参数

```
SynapseX_Host.exe [目标IP] [端口] [roi宽] [roi高]
  默认: 192.168.100.2  8888  416  416
  ROI 范围: 64–4096
```

---

## 5. 线程模型

- 主线程固定到核心 2（`SetThreadAffinityMask(1ULL << 2)`）
- `THREAD_PRIORITY_TIME_CRITICAL`
- `timeBeginPeriod(1)` — 定时器分辨率 15.6ms → 1ms
- 170Hz = 5.882ms 帧预算
- HttpTuner 在独立后台线程运行

---

## 6. 目标选择与自瞄

### 数据归一化

所有模型输出归一化为统一的 `AimPoint { cx, cy, priority, distance }`。

### 逐模型检测处理

| modelId | 游戏 | 类别 | 逻辑 |
|---------|------|------|------|
| 0 | Apex(新) | 2类: 队友/敌人 | classId==1, minConfidence; classId==0 丢弃 |
| 1 | Delta | 2类: 身体/头部 | head(classId==1, deltaHeadConf)→pri1; body(classId==0)→pri2 |
| 2 | BF6 | 2类: 敌人/队友 | classId==0, minConfidence; classId==1 丢弃 |
| 3 | OW2 | 1类: 敌人 | classId==0, minConfidence |
| 4 | Aimlabs | 1类: 敌人 | classId==0, minConfidence |
| 5 | PUBG | 2类: 身体/头部 | 同 Delta |
| 6 | CrossFire | 2类: 身体/头部 | 同 Delta |
| 7 | CS2 | 2类: CT(classId=0), T(classId=1) | classFilter筛选, 支持头身(headOffset), 同BF6风格 |

### 空间锁定

- 保持锁定半径: 80px
- 最大丢失帧数: 5 (约 29ms)
- 优先级感知: priority=1 优先于 priority=2
- 自动升级: 80px 内出现 priority=1 时从 priority=2 升级

### 游戏分辨率缩放

```
autoScaleX = gameW / screenW
autoScaleY = gameH / screenH
dx = (targetCx - screenCenterX) * autoScaleX
dy = (targetCy - screenCenterY) * autoScaleY
```

### 热键

- **PageUp**: 启用自瞄
- **PageDown**: 禁用自瞄

---

## 7. 构建

```powershell
cd host
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

依赖: CMake 3.28+, VS 2026, Windows SDK 10.0.26100, D3D11, DXGI, Winsock2, LZ4(源码编译), cpp-httplib(header-only), spdlog(header-only)

---

## 8. 已知限制

1. **独占全屏**: 无法捕获，需无边框窗口模式
2. **管理员权限**: ddll64.dll 需要
3. **反作弊**: 部分游戏主动阻止桌面复制
4. **多 GPU**: 枚举第一个有桌面输出的适配器
