// ─── Synapse-X Client — Full Pipeline main loop ────────────
//
// Pipeline:
//   UDP recv → out-of-order reassembly → LZ4 decompress →
//   BGRA (dynamic ROI) → TensorRT inference → detections
//
// Verification:
//   · Per-second FPS and drop-rate stats printed to stderr.
//   · On the 10th successfully decoded frame, saves client_test.bmp.
//   · Inference results (detections) printed per frame (limited rate).
//
// Usage:
//   .\SynapseX_Client.exe [port] [enginePath] [hostIp]
//   Default port: 8888
//   Default engine: ../../model/bf416.engine
//   Default hostIp: 192.168.100.1 (reply port: 8889)
//
// Build:
//   cd client
//   cmake --preset windows-x64
//   cmake --build build_x64 --config RelWithDebInfo

#include "UdpReceiver.h"
#include "TrtInference.h"
#include "UdpReplySender.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>

// ═══════════════════════════════════════════════════════════════
//  Global control
// ═══════════════════════════════════════════════════════════════

static std::atomic<bool> g_running{true};

// ═══════════════════════════════════════════════════════════════
//  High-precision timer helpers
// ═══════════════════════════════════════════════════════════════

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

// ═══════════════════════════════════════════════════════════════
//  Hand-rolled BMP writer
// ═══════════════════════════════════════════════════════════════

static bool SaveBgraAsBmp(const char* path,
                          const uint8_t* pixels,
                          int width,
                          int height) {
    int rowSize   = width * 4;
    int padSize   = (4 - (rowSize % 4)) % 4;
    int rowStride = rowSize + padSize;
    int imageSize = rowStride * height;

    BITMAPFILEHEADER bf = {};
    bf.bfType      = 0x4D42;
    bf.bfSize      = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + imageSize;
    bf.bfOffBits   = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);

    BITMAPINFOHEADER bi = {};
    bi.biSize        = sizeof(BITMAPINFOHEADER);
    bi.biWidth       = width;
    bi.biHeight      = -height;
    bi.biPlanes      = 1;
    bi.biBitCount    = 32;
    bi.biCompression = BI_RGB;
    bi.biSizeImage   = imageSize;

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[BMP] Cannot open file: %s\n", path);
        return false;
    }
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
//  Entry point
// ═══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    uint16_t listenPort = (argc > 1)
        ? static_cast<uint16_t>(std::atoi(argv[1]))
        : 8888;

    std::string enginePath = (argc > 2)
        ? argv[2]
        : "../../model/bf416.engine";

    std::string hostIp = (argc > 3)
        ? argv[3]
        : "192.168.100.1";

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Client — Full Pipeline\n");
    fprintf(stderr, "  Listening on: 0.0.0.0:%u\n", listenPort);
    fprintf(stderr, "  Engine path:  %s\n", enginePath.c_str());
    fprintf(stderr, "  Reply to:     %s:8889\n", hostIp.c_str());
    fprintf(stderr, "============================================\n\n");

    // ── Initialize UDP receiver ──────────────────────────
    SynapseX::UdpReceiver receiver;
    if (!receiver.Initialize(listenPort)) {
        fprintf(stderr, "[FATAL] UdpReceiver init FAILED.\n");
        return 1;
    }

    // ── Initialize TensorRT inference ────────────────────
    SynapseX::TrtInference trt;
    bool trtReady = trt.Initialize(enginePath, 416, 416, 300);
    if (!trtReady) {
        fprintf(stderr, "[WARN] TensorRT inference NOT available. "
                "Running in receive-only mode.\n");
        fprintf(stderr, "[WARN] Check engine path: %s\n", enginePath.c_str());
    }

    // ── Initialize reply sender ───────────────────────────
    SynapseX::UdpReplySender replySender;
    bool replyReady = replySender.Initialize(hostIp, 8889);
    if (!replyReady) {
        fprintf(stderr, "[WARN] Reply sender init FAILED. "
                "Running without reply channel.\n");
    }

    fprintf(stderr, "[INFO] Waiting for data from Host...\n");
    fprintf(stderr, "[INFO] Frame 10 will be saved as client_test.bmp\n");
    fprintf(stderr, "[INFO] Press Ctrl+C to stop.\n\n");

    // ── Main loop state ──────────────────────────────────
    std::vector<uint8_t> frameBuffer;

    uint32_t    receivedFrameId = 0;
    uint64_t    totalFrames     = 0;
    uint64_t    inferCount      = 0;
    bool        bmpSaved        = false;
    const char* bmpPath         = "client_test.bmp";

    TimePoint   windowStart     = Clock::now();
    TimePoint   sessionStart    = Clock::now();

    uint64_t    prevPackets     = 0;
    uint64_t    prevDropped     = 0;
    uint64_t    prevFrames      = 0;
    uint64_t    prevBytes       = 0;
    uint64_t    prevInfer       = 0;

    // ── Per-second timing accumulators ──────────────────
    double      sumRecvMs       = 0.0;
    double      sumInferMs      = 0.0;
    uint64_t    timedFrames     = 0;

    // Detection rate limiting: only print detections every N frames
    constexpr int kPrintDetEvery = 30;

    // ═══════════════════════════════════════════════════════
    //  MAIN PIPELINE LOOP
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── Stage 1: Receive & decompress ─────────────────
        auto t0 = Clock::now();
        bool gotFrame = receiver.TryReceive(frameBuffer, receivedFrameId);
        auto t1 = Clock::now();

        if (gotFrame) {
            totalFrames++;
            uint16_t roiW = receiver.GetLastFrameWidth();
            uint16_t roiH = receiver.GetLastFrameHeight();

            // ── BMP save on 10th frame ────────────────────
            if (totalFrames == 10 && !bmpSaved) {
                if (SaveBgraAsBmp(bmpPath, frameBuffer.data(), roiW, roiH)) {
                    fprintf(stderr,
                        "\n[VERIFY] Frame #%llu (Host frameId=%u) saved as '%s'\n",
                        static_cast<unsigned long long>(totalFrames),
                        receivedFrameId, bmpPath);
                    fprintf(stderr,
                        "[VERIFY] Dimensions: %dx%d, 32-bit BGRA, %zu bytes\n",
                        roiW, roiH, frameBuffer.size());
                    if (frameBuffer.size() >= 16) {
                        const uint8_t* p = frameBuffer.data();
                        fprintf(stderr, "[VERIFY] First 4 pixels (B,G,R,A): "
                                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u] "
                                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u]\n",
                                p[0],p[1],p[2],p[3],
                                p[4],p[5],p[6],p[7],
                                p[8],p[9],p[10],p[11],
                                p[12],p[13],p[14],p[15]);
                    }
                    bmpSaved = true;
                }
            }

            // ── Stage 2: TensorRT inference ───────────────
            if (trtReady) {
                // Verify frame dimensions match model input
                if (roiW == static_cast<uint16_t>(trt.GetModelWidth()) &&
                    roiH == static_cast<uint16_t>(trt.GetModelHeight())) {

                    auto t2 = Clock::now();
                    std::vector<SynapseX::Detection> dets =
                        trt.Infer(frameBuffer.data(), 0.25f);
                    auto t3 = Clock::now();
                    inferCount++;

                    // Accumulate per-frame timing
                    sumRecvMs  += ToMs(t1 - t0);
                    sumInferMs += ToMs(t3 - t2);
                    timedFrames++;

                    // ── Stage 3: Send reply to Host ───────────
                    if (replyReady && !dets.empty()) {
                        replySender.SendReplies(receivedFrameId, dets);
                    }

                    // Print detections periodically
                    if (totalFrames % kPrintDetEvery == 0 && !dets.empty()) {
                        fprintf(stderr, "[INFER] Frame #%llu: %zu detections\n",
                                static_cast<unsigned long long>(totalFrames),
                                dets.size());
                        // Print top 3
                        int show = std::min(static_cast<int>(dets.size()), 3);
                        for (int i = 0; i < show; ++i) {
                            const char* className = (dets[i].classId == 0) ? "enemy" :
                                                    (dets[i].classId == 1) ? "teammate" : "?";
                            fprintf(stderr, "  [%s] conf=%.2f box=[%.0f,%.0f,%.0f,%.0f]\n",
                                    className, dets[i].confidence,
                                    dets[i].x1, dets[i].y1,
                                    dets[i].x2, dets[i].y2);
                        }
                    }
                } else {
                    // Dimension mismatch — skip inference for this frame
                    static bool warned = false;
                    if (!warned) {
                        fprintf(stderr, "[WARN] Frame ROI %dx%d != model %dx%d. "
                                "Skipping inference. Configure Host to send "
                                "%dx%d ROI.\n",
                                roiW, roiH,
                                trt.GetModelWidth(), trt.GetModelHeight(),
                                trt.GetModelWidth(), trt.GetModelHeight());
                        warned = true;
                    }
                }
            }
        }

        // ── Per-second stats report ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            uint64_t curPackets = receiver.GetTotalPackets();
            uint64_t curDropped = receiver.GetTotalDropped();
            uint64_t curFrames  = receiver.GetTotalFrames();

            uint64_t framesThisSec  = curFrames  - prevFrames;
            uint64_t droppedThisSec = curDropped - prevDropped;
            uint64_t packetsThisSec = curPackets - prevPackets;

            double fps     = framesThisSec / elapsed;
            uint64_t totalAttempted = framesThisSec + droppedThisSec;
            double dropRate = (totalAttempted > 0)
                ? (100.0 * droppedThisSec / totalAttempted)
                : 0.0;

            uint64_t curBytes   = receiver.GetTotalBytes();
            uint64_t bytesThisSec = curBytes - prevBytes;
            double MBps = bytesThisSec / (elapsed * 1024.0 * 1024.0);

            uint64_t inferThisSec = inferCount - prevInfer;
            uint16_t roiW = receiver.GetLastFrameWidth();
            uint16_t roiH = receiver.GetLastFrameHeight();

            double avgRecvMs  = timedFrames > 0 ? sumRecvMs  / timedFrames : 0.0;
            double avgInferMs = timedFrames > 0 ? sumInferMs / timedFrames : 0.0;
            double avgTotalMs = avgRecvMs + avgInferMs;

            if (packetsThisSec > 0 || framesThisSec > 0) {
                fprintf(stderr,
                    "---- per-second stats --------------------------------\n"
                    "  ROI: %ux%u  |  FPS: %7.1f  |  frames: %5llu  |  "
                    "dropped: %5llu  |  drop rate: %5.1f%%\n"
                    "  recv: %6.2f ms  |  infer: %6.2f ms  |  total: %6.2f ms  |  "
                    "inference: %5llu/s  |  throughput: %6.2f MB/s\n",
                    roiW, roiH,
                    fps,
                    static_cast<unsigned long long>(framesThisSec),
                    static_cast<unsigned long long>(droppedThisSec),
                    dropRate,
                    avgRecvMs, avgInferMs, avgTotalMs,
                    static_cast<unsigned long long>(inferThisSec),
                    MBps);
            }

            prevFrames  = curFrames;
            prevDropped = curDropped;
            prevPackets = curPackets;
            prevBytes   = curBytes;
            prevInfer   = inferCount;

            // Reset timing accumulators each second
            sumRecvMs   = 0.0;
            sumInferMs  = 0.0;
            timedFrames = 0;

            windowStart = Clock::now();
        }

        // ── Yield ────────────────────────────────────────
        if (!gotFrame) {
            Sleep(0);
        }
    }

    // ── Final report ─────────────────────────────────────
    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;

    fprintf(stderr, "\n============================================\n");
    fprintf(stderr, "  Session Summary\n");
    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Duration:        %.1f sec\n", sessionSec);
    fprintf(stderr, "  Total frames:    %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalFrames()));
    fprintf(stderr, "  Total dropped:   %llu\n",
            static_cast<unsigned long long>(receiver.GetTotalDropped()));
    fprintf(stderr, "  TRT inferences:  %llu\n",
            static_cast<unsigned long long>(inferCount));
    fprintf(stderr, "  Total MB recv:   %.2f\n",
            receiver.GetTotalBytes() / (1024.0 * 1024.0));
    fprintf(stderr, "  Avg FPS:         %.1f\n",
            sessionSec > 0.0
                ? receiver.GetTotalFrames() / sessionSec
                : 0.0);
    fprintf(stderr, "  BMP saved:       %s\n",
            bmpSaved ? bmpPath : "NO (did not reach frame 10)");
    fprintf(stderr, "============================================\n");

    trt.Cleanup();
    replySender.Cleanup();
    receiver.Cleanup();
    return 0;
}
