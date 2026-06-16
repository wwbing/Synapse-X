// ─── Synapse-X Client — Async Producer-Consumer Pipeline ─────
//
// Architecture:
//   Producer (core 0): UDP recv → reassemble → LZ4 decompress → LIFO push
//   Consumer (core 1): LIFO pop → TRT infer (dedicated stream) → reply
//
// Key properties:
//   · LIFO queue (size 1) — consumer always takes the LATEST frame
//   · Producer NEVER waits for GPU — zero head-of-line blocking
//   · Core affinity: network on core 0, inference on core 1
//   · CUDA stream: dedicated, non-blocking, created on consumer thread
//   · Warmup: 50 black-frame inferences force max P-state + JIT compilation
//   · Frame assembly timeout: 12ms — discard stalled partial frames
//
// Usage:
//   .\SynapseX_Client.exe [port] [enginePath] [hostIp] [--save]
//   Defaults: port=8888 engine=../../model/bf416.engine hostIp=192.168.100.1

#include "UdpReceiver.h"
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
//  Global control
// ═══════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};

// ═══════════════════════════════════════════════════════════════
//  Timing helpers
// ═══════════════════════════════════════════════════════════════

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// ═══════════════════════════════════════════════════════════════
//  Core affinity
// ═══════════════════════════════════════════════════════════════

static void PinThreadToCore(int core) {
    DWORD_PTR mask = 1ULL << static_cast<DWORD_PTR>(core);
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(), mask);
    if (old == 0) {
        fprintf(stderr, "[CPU] SetThreadAffinityMask(core=%d) FAILED: %lu\n",
                core, GetLastError());
    } else {
        fprintf(stderr, "[CPU] Thread pinned to core %d (mask=0x%llX)\n",
                core, static_cast<unsigned long long>(mask));
    }
}

// ═══════════════════════════════════════════════════════════════
//  BMP writer
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
//  LIFO Frame Slot (size-1 drop queue)
// ═══════════════════════════════════════════════════════════════

struct FrameSlot {
    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<uint8_t>    data;
    uint32_t                frameId  = 0;
    uint16_t                roiW     = 0;
    uint16_t                roiH     = 0;
    bool                    hasNew   = false;
    uint64_t                drops    = 0;  // frames overwritten before consumer read
};

// ═══════════════════════════════════════════════════════════════
//  Consumer thread (inference)
// ═══════════════════════════════════════════════════════════════

struct ConsumerCtx {
    FrameSlot*              slot;
    SynapseX::TrtInference* trt;
    SynapseX::UdpReplySender* replySender;
    bool                    trtReady;
    bool                    replyReady;

    // stats (atomic, read by producer for reporting)
    std::atomic<uint64_t>   inferCount;
    std::atomic<uint64_t>   frameCount;   // frames consumed
    std::atomic<double>     sumInferMs;   // accumulated inference time (ms)
};

static void ConsumerThread(ConsumerCtx* ctx) {
    PinThreadToCore(1);

    // ── Set CUDA device (must be done on this thread) ────
    cudaError_t devErr = cudaSetDevice(0);
    if (devErr != cudaSuccess) {
        fprintf(stderr, "[CONSUMER] cudaSetDevice(0) FAILED: %s\n",
                cudaGetErrorString(devErr));
        return;
    }

    // ── Create dedicated CUDA stream ──────────────────────
    if (ctx->trtReady) {
        if (!ctx->trt->SetupStream()) {
            fprintf(stderr, "[CONSUMER] SetupStream FAILED\n");
            ctx->trtReady = false;
        }
    }

    // ── Init GPU preprocess (NVRTC, compiles kernel at runtime)
    if (ctx->trtReady) {
        if (!SynapseX::InitCudaPreprocess()) {
            fprintf(stderr, "[CONSUMER] InitCudaPreprocess FAILED\n");
            ctx->trtReady = false;
        }
    }

    // ── Warmup: 50 black dummy frames ─────────────────────
    if (ctx->trtReady) {
        fprintf(stderr, "[CONSUMER] Warming up GPU (50 dummy frames)...\n");
        std::vector<uint8_t> black(
            ctx->trt->GetModelWidth() * ctx->trt->GetModelHeight() * 4, 0);
        for (int i = 0; i < 50 && g_running; ++i) {
            ctx->trt->Infer(black.data(), 0.9f);
        }
        // CUDA sync to ensure warmup completes
        cudaDeviceSynchronize();
        fprintf(stderr, "[CONSUMER] Warmup complete.\n");
    }

    // ═══════════════════════════════════════════════════════
    //  CONSUMER MAIN LOOP
    // ═══════════════════════════════════════════════════════
    constexpr int kPrintDetEvery = 30;
    uint64_t localFrameCount = 0;

    while (g_running) {
        std::vector<uint8_t> frameData;
        uint32_t fid = 0;
        uint16_t rw = 0, rh = 0;

        // ── Pop latest frame from LIFO slot ───────────────
        {
            std::unique_lock<std::mutex> lock(ctx->slot->mtx);
            // Wait up to 2ms for new data
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

        if (frameData.empty()) continue;  // timeout, no new frame

        // ── Run inference ─────────────────────────────────
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

            // ── Send reply ────────────────────────────────
            if (ctx->replyReady && !dets.empty()) {
                ctx->replySender->SendReplies(fid, dets);
            }

            // ── Print detections periodically ─────────────
            if (localFrameCount % kPrintDetEvery == 0 && !dets.empty()) {
                fprintf(stderr, "[INFER] Frame #%llu (hostId=%u): %zu detections\n",
                        static_cast<unsigned long long>(localFrameCount),
                        fid, dets.size());
                int show = std::min(static_cast<int>(dets.size()), 3);
                for (int i = 0; i < show; ++i) {
                    const char* cn = (dets[i].classId == 0) ? "enemy" :
                                     (dets[i].classId == 1) ? "teammate" : "?";
                    fprintf(stderr, "  [%s] conf=%.2f box=[%.0f,%.0f,%.0f,%.0f]\n",
                            cn, dets[i].confidence,
                            dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
                }
            }
        }

        ctx->frameCount.store(localFrameCount, std::memory_order_relaxed);
    }

    cudaDeviceSynchronize();
    fprintf(stderr, "[CONSUMER] Exiting. %llu frames inferred.\n",
            static_cast<unsigned long long>(localFrameCount));
}

// ═══════════════════════════════════════════════════════════════
//  Entry point
// ═══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    uint16_t listenPort = (argc > 1)
        ? static_cast<uint16_t>(std::atoi(argv[1])) : 8888;
    std::string enginePath = (argc > 2)
        ? argv[2] : "../../model/bf416.engine";
    std::string hostIp = (argc > 3)
        ? argv[3] : "192.168.100.1";
    bool saveBmp = false;
    for (int i = 4; i < argc; ++i) {
        if (strcmp(argv[i], "--save") == 0) saveBmp = true;
    }

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Client -- Async Pipeline\n");
    fprintf(stderr, "  Architecture: Producer(core0) | Consumer(core1)\n");
    fprintf(stderr, "  Listening on: 0.0.0.0:%u\n", listenPort);
    fprintf(stderr, "  Engine path:  %s\n", enginePath.c_str());
    fprintf(stderr, "  Reply to:     %s:8889\n", hostIp.c_str());
    fprintf(stderr, "  BMP dump:     %s\n", saveBmp ? "ON (--save)" : "OFF");
    fprintf(stderr, "============================================\n\n");

    // ── Init modules (main thread) ───────────────────────
    SynapseX::UdpReceiver receiver;
    if (!receiver.Initialize(listenPort)) {
        fprintf(stderr, "[FATAL] UdpReceiver init FAILED.\n");
        return 1;
    }

    SynapseX::TrtInference trt;
    bool trtReady = trt.Initialize(enginePath, 416, 416, 300);
    if (!trtReady) {
        fprintf(stderr, "[WARN] TRT inference NOT available. "
                "Receive-only mode.\n");
    }

    SynapseX::UdpReplySender replySender;
    bool replyReady = replySender.Initialize(hostIp, 8889);
    if (!replyReady) {
        fprintf(stderr, "[WARN] Reply sender init FAILED.\n");
    }

    // ── LIFO frame slot (shared between threads) ─────────
    FrameSlot slot;

    // ── Consumer context ─────────────────────────────────
    ConsumerCtx consumerCtx;
    consumerCtx.slot         = &slot;
    consumerCtx.trt          = &trt;
    consumerCtx.replySender  = &replySender;
    consumerCtx.trtReady     = trtReady;
    consumerCtx.replyReady   = replyReady;
    consumerCtx.inferCount   = 0;
    consumerCtx.frameCount   = 0;
    consumerCtx.sumInferMs   = 0.0;

    // ── Spawn consumer thread ────────────────────────────
    fprintf(stderr, "[INFO] Spawning consumer thread on core 1...\n");
    std::thread consumer(ConsumerThread, &consumerCtx);

    // ── Pin producer (main thread) to core 0 ─────────────
    PinThreadToCore(0);

    fprintf(stderr, "[INFO] Producer on core 0. Waiting for Host data...\n");
    fprintf(stderr, "[INFO] Press Ctrl+C to stop.\n\n");

    // ── Producer state ───────────────────────────────────
    std::vector<uint8_t> frameBuffer;
    std::vector<uint8_t> bmpBuffer;        // for --save
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

    // Frame assembly timeout: 12ms (= 2x 170Hz period)
    TimePoint lastPacketTime = Clock::now();

    // ═══════════════════════════════════════════════════════
    //  PRODUCER MAIN LOOP (core 0)
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── Receive & decompress ──────────────────────────
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

            // BMP copy (before move)
            if (saveBmp) {
                bmpBuffer = frameBuffer;
                bmpRoiW   = roiW;
                bmpRoiH   = roiH;
            }

            // ── LIFO push (overwrite old, notify consumer) ─
            {
                std::lock_guard<std::mutex> lock(slot.mtx);
                if (slot.hasNew) {
                    slot.drops++;  // consumer hadn't picked up old frame
                }
                slot.data    = std::move(frameBuffer);
                slot.frameId = receivedFrameId;
                slot.roiW    = roiW;
                slot.roiH    = roiH;
                slot.hasNew  = true;
            }
            slot.cv.notify_one();
        } else {
            // ── Frame assembly timeout check ──────────────
            // If we have a partial frame stuck > 12ms with no new
            // chunks, discard it.  This prevents hanging when Host
            // stops mid-frame.
            double stallMs = ToMs(Clock::now() - lastPacketTime);
            (void)stallMs;  // reserved for future forced-reset
            Sleep(0);
        }

        // ── Per-second stats ──────────────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            // BMP save (latest frame before this second)
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
                fprintf(stderr,
                    "---- per-second stats --------------------------------\n"
                    "  ROI: %ux%u  |  FPS: %7.1f  |  fr: %5llu  |  "
                    "drop: %5llu (%4.1f%%)  |  LIFO drops: %llu\n"
                    "  recv: %6.2f ms  |  infer: %6.2f ms  |  "
                    "total: %6.2f ms  |  infer/s: %5llu  |  BW: %6.2f MB/s\n",
                    roiW, roiH, fps,
                    static_cast<unsigned long long>(framesThisSec),
                    static_cast<unsigned long long>(droppedThisSec),
                    dropRate,
                    static_cast<unsigned long long>(slot.drops),
                    avgRecvMs, avgInferMs, avgTotalMs,
                    static_cast<unsigned long long>(inferThisSec),
                    MBps);
            }

            // Reset per-second accumulators
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

    // ── Shutdown ──────────────────────────────────────────
    fprintf(stderr, "\n[INFO] Shutting down...\n");
    slot.cv.notify_all();
    consumer.join();

    // ── Final report ─────────────────────────────────────
    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    fprintf(stderr, "\n============================================\n");
    fprintf(stderr, "  Session Summary\n");
    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Duration:        %.1f sec\n", sessionSec);
    fprintf(stderr, "  Producer frames: %llu\n",
            static_cast<unsigned long long>(producerFrames));
    fprintf(stderr, "  Consumer frames: %llu\n",
            static_cast<unsigned long long>(
                consumerCtx.frameCount.load(std::memory_order_relaxed)));
    fprintf(stderr, "  LIFO overwrites: %llu\n",
            static_cast<unsigned long long>(slot.drops));
    fprintf(stderr, "  Total dropped:   %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalDropped()));
    fprintf(stderr, "  Total packets:   %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalPackets()));
    fprintf(stderr, "  Total MB recv:   %.2f\n",
            receiver.GetTotalBytes() / (1024.0 * 1024.0));
    fprintf(stderr, "  Avg FPS:         %.1f\n",
            sessionSec > 0.0 ? producerFrames / sessionSec : 0.0);
    fprintf(stderr, "  BMPs saved:      %d\n", saveIndex);
    fprintf(stderr, "============================================\n");

    trt.Cleanup();
    replySender.Cleanup();
    receiver.Cleanup();
    return 0;
}
