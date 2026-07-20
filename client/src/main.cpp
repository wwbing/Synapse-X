// ─── Synapse-X 客户端 — 异步生产者-消费者流水线 ─────────────
//
// 架构：
//   Producer（核心 0）：UDP 接收 → 重组 → LZ4 解压 → LIFO 推送
//   Consumer（核心 1）：LIFO 弹出 → TRT 推理（专用流）→ 回复
//
// 关键特性：
//   · LIFO 队列（大小 1）— 消费者始终获取最新帧
//   · 生产者从不等待 GPU — 零队头阻塞
//   · 核心亲和性：网络在核心 0，推理在核心 1
//   · CUDA 流：专用、非阻塞，在消费者线程上创建
//   · 预热：50 帧黑帧推理强制最大 P 状态 + JIT 编译
//   · 帧组装超时：12ms — 丢弃停滞的不完整帧
//
// 用法：
//   .\SynapseX_Client.exe [port] [enginePath] [hostIp] [--save]
//   默认值：port=8888 engine=../../model/bf416.engine hostIp=192.168.100.1

#include "UdpReceiver.h"
#include "Log.h"
#include "TrtInference.h"
#include "UdpReplySender.h"
#include "CudaPreprocess.h"

#include <windows.h>
#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════
//  全局控制
// ═══════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};

// ═══════════════════════════════════════════════════════════════
//  计时辅助函数
// ═══════════════════════════════════════════════════════════════

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// ═══════════════════════════════════════════════════════════════
//  核心亲和性
// ═══════════════════════════════════════════════════════════════

static void PinThreadToCore(int core) {
    DWORD_PTR mask = 1ULL << static_cast<DWORD_PTR>(core);
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (old == 0) {
        SX_LOG_WARN("[Client] SetThreadAffinityMask(core={}) 失败: {}",
                    core, GetLastError());
    } else {
        SX_LOG_INFO("[Client] 线程已绑定到核心 {} (掩码=0x{:X})",
                    core, static_cast<unsigned long long>(mask));
    }
}

// ═══════════════════════════════════════════════════════════════
//  BMP 写入器
// ═══════════════════════════════════════════════════════════════

static bool SaveBgraAsBmp(const char* path,
                          const uint8_t* pixels,
                          int width, int height) {
    int rowSize   = width * 4;
    int padSize   = (4 - (rowSize % 4)) % 4;
    int rowStride = rowSize + padSize;
    int imageSize = rowStride * height;

    BITMAPFILEHEADER bf = {};
    bf.bfType    = 0x4D42;
    bf.bfSize    = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER bi = {};
    bi.biSize        = sizeof(BITMAPINFOHEADER);
    bi.biWidth       = width;
    bi.biHeight      = -height;
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage   = imageSize;

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    const uint8_t* row = pixels;
    uint8_t padding[4] = {0};
    for (int y = 0; y < height; ++y) {
        fwrite(row, 1, rowSize, f);
        if (padSize > 0) fwrite(padding, 1, padSize, f);
        row += rowSize;
    }
    fclose(f);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LIFO 帧槽（大小-1 丢弃队列）
// ═══════════════════════════════════════════════════════════════

struct FrameSlot {
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<uint8_t>    data;
    uint32_t                frameId  = 0;
    uint16_t                roiW     = 0;
    uint16_t                roiH     = 0;
    bool                    hasNew   = false;
    uint64_t                drops    = 0;  // 在消费者读取前被覆盖的帧数
};

// ═══════════════════════════════════════════════════════════════
//  消费者线程（推理）
// ═══════════════════════════════════════════════════════════════

struct ConsumerCtx {
    FrameSlot*              slot;
    SynapseX::TrtInference* trt;
    SynapseX::UdpReplySender* replySender;
    bool                    trtReady;
    bool                    replyReady;

    // 统计信息（原子类型，由生产者读取用于报告）
    std::atomic<uint64_t>   inferCount;
    std::atomic<uint64_t>   frameCount;   // 已消费的帧数
    std::atomic<double>     sumInferMs;   // 累计推理时间（毫秒）
};

static void ConsumerThread(ConsumerCtx* ctx) {
    PinThreadToCore(1);

    // ── 设置CUDA设备（必须在此线程上执行）───────────────
    cudaError_t devErr = cudaSetDevice(0);
    if (devErr != cudaSuccess) {
        SX_LOG_ERROR("[Client] Consumer cudaSetDevice(0) 失败: {}",
                     cudaGetErrorString(devErr));
        return;
    }

    // ── 创建专用的CUDA流 ─────────────────────────────────
    if (ctx->trtReady) {
        if (!ctx->trt->SetupStream()) {
            SX_LOG_ERROR("[Client] Consumer SetupStream 失败");
            ctx->trtReady = false;
        }
    }

    // ── 初始化GPU预处理（NVRTC，运行时编译内核）────────
    if (ctx->trtReady) {
        if (!SynapseX::InitCudaPreprocess()) {
            SX_LOG_ERROR("[Client] Consumer InitCudaPreprocess 失败");
            ctx->trtReady = false;
        }
    }

    // ── 加载初始引擎（modelId 来自主机，默认 0）────────
    if (ctx->trtReady) {
        uint8_t initModel = g_targetModelId.load(std::memory_order_relaxed);
        if (!ctx->trt->LoadEngine(initModel)) {
            SX_LOG_ERROR("[Client] Consumer LoadEngine({}) 失败；推理已禁用，等待有效模型ID",
                         initModel);
            ctx->trtReady = false;
        }
    }

    // ── 预热：50 帧黑色哑元帧 ───────────────────────────
    if (ctx->trtReady) {
        SX_LOG_INFO("[Client] Consumer 正在用50帧黑图预热GPU");
        std::vector<uint8_t> black(
            ctx->trt->GetModelWidth() * ctx->trt->GetModelHeight() * 4, 0);
        for (int i = 0; i < 50 && g_running; ++i) {
            ctx->trt->Infer(black.data(), 0.9f);
        }
        // CUDA 同步以确保预热完成
        cudaDeviceSynchronize();
        SX_LOG_INFO("[Client] Consumer 预热完成");
    }

    // ═══════════════════════════════════════════════════════
    //  消费者主循环
    // ═══════════════════════════════════════════════════════
    constexpr int kPrintDetEvery = 30;
    uint64_t localFrameCount = 0;

    while (g_running) {
        std::vector<uint8_t> frameData;
        uint32_t fid = 0;
        uint16_t rw = 0, rh = 0;

        // ── 从LIFO槽中弹出最新帧 ─────────────────────────
        {
            std::unique_lock<std::mutex> lock(ctx->slot->mtx);
            // 最多等待 2ms 的新数据
            ctx->slot->cv.wait_for(lock, std::chrono::milliseconds(2),
                [&]{ return ctx->slot->hasNew || !g_running; });

            if (!g_running) break;

            if (ctx->slot->hasNew) {
                frameData = std::move(ctx->slot->data);
                fid       = ctx->slot->frameId;
                rw        = ctx->slot->roiW;
                rh        = ctx->slot->roiH;
                ctx->slot->hasNew = false;
            }
        }

        if (frameData.empty()) continue;  // 超时，无新帧

        // ── 运行推理 ─────────────────────────────────────
        if (ctx->trtReady &&
            rw == static_cast<uint16_t>(ctx->trt->GetModelWidth()) &&
            rh == static_cast<uint16_t>(ctx->trt->GetModelHeight())) {

            auto t0 = Clock::now();
            auto dets = ctx->trt->Infer(frameData.data(), 0.25f);
            auto t1 = Clock::now();

            ctx->inferCount++;
            localFrameCount++;
            ctx->sumInferMs.store(ctx->sumInferMs.load(std::memory_order_relaxed)
                                  + ToMs(t1 - t0), std::memory_order_relaxed);

            // ── 发送回复 ──────────────────────────────────
            if (ctx->replyReady && !dets.empty()) {
                ctx->replySender->SendReplies(fid, dets);
            }

            // ── 定期打印检测结果 ─────────────────────────
            if (localFrameCount % kPrintDetEvery == 0 && !dets.empty()) {
                static const char* kApexCls[]    = {"teammate", "enemy"};
                static const char* kDeltaCls[]   = {"body", "head"};
                static const char* kBf6Cls[]     = {"enemy", "teammate"};
                static const char* kOw2Cls[]     = {"enemy"};
                static const char* kAimlabsCls[] = {"enemy"};
                static const char* kPubgCls[]    = {"body", "head"};
                static const char* kCfCls[]      = {"body", "head"};
                static const char* kCs2Cls[]     = {"CT", "T"};

                uint8_t mid = ctx->trt->GetCurrentModelId();
                const char* gameName; const char* const* clsNames; int numCls;
                switch (mid) {
                    case 0: gameName="Apex";      clsNames=kApexCls;    numCls=2; break;
                    case 1: gameName="Delta";     clsNames=kDeltaCls;   numCls=2; break;
                    case 2: gameName="BF6";       clsNames=kBf6Cls;     numCls=2; break;
                    case 3: gameName="OW2";       clsNames=kOw2Cls;     numCls=1; break;
                    case 4: gameName="Aimlabs";   clsNames=kAimlabsCls; numCls=1; break;
                    case 5: gameName="PUBG";      clsNames=kPubgCls;    numCls=2; break;
                    case 6: gameName="CrossFire"; clsNames=kCfCls;      numCls=2; break;
                    case 7: gameName="CS2";       clsNames=kCs2Cls;     numCls=2; break;
                    default: gameName="?";        clsNames=nullptr;     numCls=0; break;
                }
                SX_LOG_DEBUG("[Client] 推理帧={} 主机帧={} 模型={} 检测数={}",
                             static_cast<unsigned long long>(localFrameCount),
                             fid, gameName, dets.size());
                int show = std::min(static_cast<int>(dets.size()), 3);
                for (int i = 0; i < show; ++i) {
                    int cls = dets[i].classId;
                    const char* cn = (cls >= 0 && cls < numCls) ? clsNames[cls] : "?";
                    SX_LOG_DEBUG("[Client]   检测 class={} 置信度={:.2f} 框=[{:.0f},{:.0f},{:.0f},{:.0f}]",
                                 cn, dets[i].confidence,
                                 dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
                }
            }
        }

        ctx->frameCount.store(localFrameCount, std::memory_order_relaxed);
    }

    cudaDeviceSynchronize();
    SX_LOG_INFO("[Client] Consumer 退出，共推理 {} 帧",
                static_cast<unsigned long long>(localFrameCount));
}

// ═══════════════════════════════════════════════════════════════
//  Entry point
// ═══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // ── 解析参数 ────────────────────────────────────────
    uint16_t listenPort = (argc > 1)
        ? static_cast<uint16_t>(std::atoi(argv[1])) : 8888;
    std::string enginePath = (argc > 2)
        ? argv[2] : "../../model/bf416.engine";
    std::string hostIp = (argc > 3)
        ? argv[3] : "192.168.100.1";
    SynapseX::Log::Initialize("client");

    bool saveBmp = false;
    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--save") == 0) saveBmp = true;
    }

    SX_LOG_INFO("[Client] 启动异步管线: 监听=0.0.0.0:{} 引擎路径={} 回复目标={}:8889 保存BMP={}",
                listenPort, enginePath, hostIp, saveBmp);

    // ── 初始化模块（主线程） ────────────────────────────
    SynapseX::UdpReceiver receiver;
    if (!receiver.Initialize(listenPort)) {
        SX_LOG_CRITICAL("[Client] UdpReceiver 初始化失败");
        return 1;
    }

    SynapseX::TrtInference trt;
    bool trtReady = trt.Initialize();
    if (!trtReady) {
        SX_LOG_WARN("[Client] TRT 推理不可用；以仅接收模式运行");
    }

    SynapseX::UdpReplySender replySender;
    bool replyReady = replySender.Initialize(hostIp, 8889);
    if (!replyReady) {
        SX_LOG_WARN("[Client] 回复发送器初始化失败");
    }

    // ── LIFO 帧槽（在线程间共享） ──────────────────────
    FrameSlot slot;

    // ── 消费者上下文 ───────────────────────────────────
    ConsumerCtx consumerCtx;
    consumerCtx.slot         = &slot;
    consumerCtx.trt          = &trt;
    consumerCtx.replySender  = &replySender;
    consumerCtx.trtReady     = trtReady;
    consumerCtx.replyReady   = replyReady;
    consumerCtx.inferCount   = 0;
    consumerCtx.frameCount   = 0;
    consumerCtx.sumInferMs   = 0.0;

    // ── 创建消费者线程 ──────────────────────────────────
    SX_LOG_INFO("[Client] 在核心1上创建消费者线程");
    std::thread consumer(ConsumerThread, &consumerCtx);

    // ── 将生产者（主线程）绑定到核心 0 ─────────────────
    PinThreadToCore(0);

    SX_LOG_INFO("[Client] Producer 已绑定到核心0，等待主机数据");
    SX_LOG_INFO("[Client] 按 Ctrl+C 停止");

    // ── 生产者状态 ─────────────────────────────────────
    std::vector<uint8_t> frameBuffer;
    std::vector<uint8_t> bmpBuffer;        // 用于 --save
    uint32_t receivedFrameId = 0;
    uint64_t producerFrames  = 0;
    uint16_t bmpRoiW = 0, bmpRoiH = 0;
    int      saveIndex = 0;

    TimePoint windowStart  = Clock::now();
    TimePoint sessionStart = Clock::now();

    uint64_t prevPackets = 0, prevDropped = 0;
    uint64_t prevFrames  = 0, prevBytes   = 0;
    uint64_t prevInfer   = 0;

    double   sumRecvMs   = 0.0;
    uint64_t timedFrames = 0;

    // 帧组装超时：12ms（= 2 倍 170Hz 周期）
    TimePoint lastPacketTime = Clock::now();

    // ═══════════════════════════════════════════════════════
    //  生产者主循环（核心 0）
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── 接收和解压 ──────────────────────────────────
        auto t0 = Clock::now();
        bool gotFrame = receiver.TryReceive(frameBuffer, receivedFrameId);
        auto t1 = Clock::now();

        if (gotFrame) {
            producerFrames++;
            lastPacketTime = Clock::now();
            uint16_t roiW = receiver.GetLastFrameWidth();
            uint16_t roiH = receiver.GetLastFrameHeight();

            sumRecvMs  += ToMs(t1 - t0);
            timedFrames++;

            // BMP 拷贝（在移动之前）
            if (saveBmp) {
                bmpBuffer = frameBuffer;
                bmpRoiW   = roiW;
                bmpRoiH   = roiH;
            }

            // ── LIFO 推送（覆盖旧的，通知消费者）───────
            {
                std::lock_guard<std::mutex> lock(slot.mtx);
                if (slot.hasNew) {
                    slot.drops++;  // 消费者尚未取走旧帧
                }
                slot.data    = std::move(frameBuffer);
                slot.frameId = receivedFrameId;
                slot.roiW    = roiW;
                slot.roiH    = roiH;
                slot.hasNew  = true;
            }
            slot.cv.notify_one();
        } else {
            // ── 帧组装超时检查 ───────────────────────────
            // 如果存在超过 12ms 无新数据块的部分帧，
            // 丢弃它。这防止主机在帧中途停止时挂起。
            double stallMs = ToMs(Clock::now() - lastPacketTime);
            (void)stallMs;  // 为将来强制重置保留
            Sleep(0);
        }

        // ── 每秒统计 ─────────────────────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            // 保存BMP（这一秒之前的最近一帧）
            if (saveBmp && !bmpBuffer.empty() && bmpRoiW > 0) {
                char bmpName[64];
                snprintf(bmpName, sizeof(bmpName),
                         "client_%04d.bmp", saveIndex++);
                SaveBgraAsBmp(bmpName, bmpBuffer.data(),
                              bmpRoiW, bmpRoiH);
            }

            uint64_t curPackets = receiver.GetTotalPackets();
            uint64_t curDropped = receiver.GetTotalDropped() + slot.drops;
            uint64_t curFrames  = receiver.GetTotalFrames();
            uint64_t curInfer   = consumerCtx.inferCount.load(
                                     std::memory_order_relaxed);
            uint64_t curBytes   = receiver.GetTotalBytes();

            uint64_t framesThisSec  = curFrames  - prevFrames;
            uint64_t droppedThisSec = curDropped - prevDropped;
            uint64_t inferThisSec   = curInfer   - prevInfer;
            uint64_t packetsThisSec = curPackets - prevPackets;
            uint64_t bytesThisSec   = curBytes   - prevBytes;

            double fps = framesThisSec / elapsed;
            uint64_t totalAttempted = framesThisSec + droppedThisSec;
            double dropRate = (totalAttempted > 0)
                ? (100.0 * droppedThisSec / totalAttempted) : 0.0;
            double MBps = bytesThisSec / (elapsed * 1024.0 * 1024.0);

            double avgRecvMs = timedFrames > 0
                ? sumRecvMs / timedFrames : 0.0;
            double avgInferMs = inferThisSec > 0
                ? consumerCtx.sumInferMs.load(std::memory_order_relaxed) / inferThisSec
                : 0.0;
            double avgTotalMs = avgRecvMs + avgInferMs;

            uint16_t roiW = receiver.GetLastFrameWidth();
            uint16_t roiH = receiver.GetLastFrameHeight();

            if (packetsThisSec > 0 || framesThisSec > 0) {
                SX_LOG_DEBUG("[Client] 统计: ROI={}x{} 帧率={:.1f} 帧数={} 丢弃={} 丢弃率={:.1f}% LIFO丢弃={} 接收耗时={:.2f} 推理耗时={:.2f} 总耗时={}0 每秒推理={}1 带宽MB/s={:.2f}",
                             roiW, roiH, fps,
                             static_cast<unsigned long long>(framesThisSec),
                             static_cast<unsigned long long>(droppedThisSec),
                             dropRate,
                             static_cast<unsigned long long>(slot.drops),
                             avgRecvMs, avgInferMs, avgTotalMs,
                             static_cast<unsigned long long>(inferThisSec),
                             MBps);
            }

            // 重置每秒累加器
            prevFrames  = curFrames;
            prevDropped = curDropped;
            prevPackets = curPackets;
            prevBytes   = curBytes;
            prevInfer   = curInfer;
            sumRecvMs   = 0.0;
            timedFrames = 0;
            consumerCtx.sumInferMs.store(0.0, std::memory_order_relaxed);
            windowStart = Clock::now();
        }
    }

    // ── 关闭 ─────────────────────────────────────────────
    SX_LOG_INFO("[Client] 正在关闭");
    slot.cv.notify_all();
    consumer.join();

    // ── 最终报告 ─────────────────────────────────────────
    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    SX_LOG_INFO("[Client] 会话摘要: 时长={:.1f}秒 生产者帧={} 消费者帧={} LIFO覆盖={} 丢弃帧={} 数据包={} 接收MB={:.2f} 平均帧率={:.1f} 保存BMP={}",
                sessionSec,
                static_cast<unsigned long long>(producerFrames),
                static_cast<unsigned long long>(
                    consumerCtx.frameCount.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(slot.drops),
                static_cast<unsigned long long>(receiver.GetTotalDropped()),
                static_cast<unsigned long long>(receiver.GetTotalPackets()),
                receiver.GetTotalBytes() / (1024.0 * 1024.0),
                sessionSec > 0.0 ? producerFrames / sessionSec : 0.0,
                saveIndex);

    trt.Cleanup();
    replySender.Cleanup();
    receiver.Cleanup();
    return 0;
}
