# Host 端回复接收任务

> **To**: Host 端开发者
> **From**: Client 端（副机）
> **Status**: Client 端已完成全链路，正在发送检测结果到你的 UDP 8889 端口

---

## 你需要做什么

Host 端新增一个 UDP 回复接收模块，监听 `8889` 端口，接收 Client 发回的检测坐标，
映射到屏幕坐标后在 overlay 上画框（或输出到控制台调试）。

---

## 协议（已定义在 shared/include/ReplyPacket.h）

Client 发回的每个 UDP 数据报格式如下（小端，x64 原序）：

```
┌──────────────────────┬───────────────────────────────────┐
│    ReplyHeader       │       DetectionRaw[]              │
│     16 bytes         │     numDets × 24 bytes             │
└──────────────────────┴───────────────────────────────────┘
```

### ReplyHeader (16 bytes, packed)

```
Offset  Size  Field
  0      2    magic      = 0x5359 ('SY')
  2      4    frameId    匹配被推理的帧（=Host 发送时的 frameId）
  6      2    numDets    本条回复包含的检测数量 (0–50)
  8      8    padding    预留
```

### DetectionRaw (24 bytes, packed)

```
Offset  Size  Field
  0      4    x1         左上角 X (float32, 模型像素空间)
  4      4    y1         左上角 Y
  8      4    x2         右下角 X
 12      4    y2         右下角 Y
 16      4    confidence 置信度 [0, 1]
 20      4    classId    0 = enemy, 1 = teammate
```

### 解析伪代码

```cpp
#include "ReplyPacket.h"  // shared/include/ReplyPacket.h

void HandleReply(const uint8_t* data, int len) {
    if (len < sizeof(SynapseX::ReplyHeader)) return;

    auto* header = reinterpret_cast<const SynapseX::ReplyHeader*>(data);
    if (header->magic != SynapseX::REPLY_MAGIC) return;  // 0x5359

    int expectedLen = sizeof(SynapseX::ReplyHeader)
                    + header->numDets * sizeof(SynapseX::DetectionRaw);
    if (len < expectedLen) return;

    auto* dets = reinterpret_cast<const SynapseX::DetectionRaw*>(
        data + sizeof(SynapseX::ReplyHeader));

    for (uint16_t i = 0; i < header->numDets; ++i) {
        float x1 = dets[i].x1;
        float y1 = dets[i].y1;
        float x2 = dets[i].x2;
        float y2 = dets[i].y2;
        float conf = dets[i].confidence;
        int   cls  = static_cast<int>(dets[i].classId);

        // 坐标映射到屏幕（见下方）
    }
}
```

---

## 坐标映射

Client 返回的坐标在 **模型像素空间** `[0, 415] × [0, 415]`。
Host 端需要加回 ROI 偏移量：

```
已知:
  roiX = (screenW - 416) / 2    // Host 中心裁剪的 X 偏移
  roiY = (screenH - 416) / 2    // Host 中心裁剪的 Y 偏移

对于每个检测:
  screen_x1 = roiX + x1
  screen_y1 = roiY + y1
  screen_x2 = roiX + x2
  screen_y2 = roiY + y2
```

---

## 实现步骤

### 1. 在 Host CMakeLists.txt 中加源文件

```
src/UdpReplyReceiver.cpp   ← 新增
```

无需额外链接库 — `ws2_32` 已经链接了。

### 2. include 路径（已有）

Host 已经引用了 `shared/include/`，`ReplyPacket.h` 可以直接 `#include`。

### 3. UdpReplyReceiver 实现要点

- 创建 UDP socket，绑定 `INADDR_ANY:8889`
- 设为 non-blocking 或独立线程 recvfrom
- 集成到主循环：每次 `CaptureFrame → Compress → Send` 之后，顺便 `ReceiveReply`
- 根据 `frameId` 匹配（收到的 `frameId` 对应当前帧或上一帧的检测结果）

### 4. 调试：先打印检测结果

最开始不要画框，先在控制台打印，确认能收到数据：

```
[Reply] frameId=53398 numDets=6
  enemy  conf=0.84  screen=[1147,36,1165,87]
  enemy  conf=0.72  screen=[971,281,983,311]
  enemy  conf=0.70  screen=[858,276,866,302]
```

确认坐标合理后，再接 overlay 画框。

---

## 测试验证

```powershell
# Client 端
.\SynapseX_Client.exe 8888 ..\..\model\bf416.engine 192.168.100.1

# Host 端（你已有的）
.\SynapseX_Host.exe 192.168.100.2 8888 416 416

# Host 端（新增回复监听）在另一个线程或循环中接收 UDP 8889
```

---

## 关键数字

| 参数 | 值 |
|-------|-----|
| 回复端口 | UDP 8889 |
| magic | `0x5359` |
| 回复频率 | 每帧推理完就发（有空检测才发） |
| 单包大小 | 16 + N×24 bytes（6 个检测 ≈ 160 bytes） |
| 最大检测数 | 50/帧（超过截断） |
| 坐标空间 | 模型像素 [0, 415]，Host 自己映射到屏幕 |
| 推理延迟 | ~3.5 ms（Client 端 GPU 推理） |

---

## 参考文件

- `shared/include/ReplyPacket.h` — 协议结构体（直接 include）
- `client/CLIENT_SPEC.md` §7 — 回复通道详细说明
- `host/HOST_SPEC.md` — Host 端规格（已有）

有问题看 Client 端日志行：
```
[UdpReplySender] Ready — sending to 192.168.100.1:8889
```
看到这行说明 Client 回复通道就绪。Host 没收到数据的话检查防火墙和网线。
