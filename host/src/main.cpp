// ─── Synapse-X Host — Production main loop ─────────────────
//
// Pipeline (170 Hz target):
//   DXGI center-ROI capture  →  LZ4 compress  →  UDP fragment & send
//
// Stats printed every second: actual FPS + per-stage avg latency.

#include "DxgiCapturer.h"
#include "Lz4Compressor.h"
#include "UdpSender.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>

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
//  Per-second statistics accumulator
// ═══════════════════════════════════════════════════════════════

struct PerfStats {
    int    captured    = 0;   // frames with new content
    int    sent        = 0;   // frames successfully sent
    double sumCapture  = 0.0;
    double sumCompress = 0.0;
    double sumSend     = 0.0;

    void reset() {
        captured    = 0;
        sent        = 0;
        sumCapture  = 0.0;
        sumCompress = 0.0;
        sumSend     = 0.0;
    }
};

// ═══════════════════════════════════════════════════════════════
//  Entry point
// ═══════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    const char* targetIp  = (argc > 1) ? argv[1] : "127.0.0.1";
    uint16_t    targetPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8888;

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Host -- Production Pipeline\n");
    fprintf(stderr, "  Target: %s:%u\n", targetIp, targetPort);
    fprintf(stderr, "  Target FPS: 170 Hz\n");
    fprintf(stderr, "============================================\n\n");

    // ── Stage 0: Init all modules ────────────────────────
    constexpr int ROI_W = 640;
    constexpr int ROI_H = 640;
    constexpr int RAW_SIZE = ROI_W * ROI_H * 4;  // 1,638,400 bytes

    // DXGI capturer
    SynapseX::DxgiCapturer capturer;
    if (!capturer.Initialize(ROI_W, ROI_H)) {
        fprintf(stderr, "[FATAL] DxgiCapturer init FAILED.\n");
        return 1;
    }

    // LZ4 compressor (pre-allocates worst-case buffer)
    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(RAW_SIZE)) {
        fprintf(stderr, "[FATAL] Lz4Compressor init FAILED.\n");
        return 1;
    }

    // UDP sender
    SynapseX::UdpSender sender;
    if (!sender.Initialize(targetIp, targetPort)) {
        fprintf(stderr, "[FATAL] UdpSender init FAILED.\n");
        return 1;
    }

    fprintf(stderr, "[INFO] All modules initialized. Starting main loop...\n");
    fprintf(stderr, "[INFO] Press Ctrl+C to stop.\n\n");

    // ── Main loop state ──────────────────────────────────
    std::vector<uint8_t> rawBuffer;       // BGRA frame (1.6 MB)
    std::vector<uint8_t> compressedBuffer; // LZ4 output (variable)
    rawBuffer.reserve(RAW_SIZE);
    compressedBuffer.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(RAW_SIZE));

    uint32_t    frameId       = 0;
    PerfStats   stats;
    TimePoint   windowStart   = Clock::now();
    int64_t     totalSent     = 0;
    TimePoint   sessionStart  = Clock::now();

    // ═══════════════════════════════════════════════════════
    //  MAIN LOOP
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── Stage 1: Capture ─────────────────────────────
        auto t0 = Clock::now();
        bool gotFrame = capturer.CaptureFrame(rawBuffer);
        auto t1 = Clock::now();

        if (gotFrame) {
            // ── Stage 2: Compress ────────────────────────
            auto t2a = Clock::now();
            bool ok = compressor.Compress(rawBuffer.data(),
                                           static_cast<int>(rawBuffer.size()),
                                           compressedBuffer);
            auto t2b = Clock::now();

            if (ok) {
                // ── Stage 3: Fragment & Send ─────────────
                auto t3a = Clock::now();
                bool sent = sender.SendCompressedFrame(
                    compressedBuffer.data(),
                    static_cast<uint32_t>(compressedBuffer.size()),
                    frameId);
                auto t3b = Clock::now();

                if (sent) totalSent++;

                stats.captured++;
                stats.sent       += (sent ? 1 : 0);
                stats.sumCapture  += ToMs(t1  - t0);
                stats.sumCompress += ToMs(t2b - t2a);
                stats.sumSend     += ToMs(t3b - t3a);
                frameId++;
            }
        } else {
            // No new frame — yield to OS scheduler.
            // Prevents burning CPU at millions of iterations/sec.
            std::this_thread::yield();
        }

        // ── Per-second stats report ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            double fps        = stats.captured / elapsed;
            double avgCapture = stats.captured > 0 ? stats.sumCapture  / stats.captured : 0.0;
            double avgCompress= stats.captured > 0 ? stats.sumCompress / stats.captured : 0.0;
            double avgSend    = stats.sent     > 0 ? stats.sumSend     / stats.sent     : 0.0;
            double avgTotal   = avgCapture + avgCompress + avgSend;

            fprintf(stderr,
                "---- per-second stats --------------------------------\n"
                "  FPS: %7.1f  |  sent: %5d / %5d frames  |  total: %lld\n"
                "  capture: %8.3f ms  |  compress: %8.3f ms  |  send: %8.3f ms\n"
                "  pipeline total: %5.2f ms  (budget: 5.88 ms @170Hz)\n",
                fps, stats.sent, stats.captured, (long long)totalSent,
                avgCapture, avgCompress, avgSend,
                avgTotal);

            stats.reset();
            windowStart = Clock::now();
        }
    }

    // ── Final report ─────────────────────────────────────
    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    fprintf(stderr, "\n[DONE] Session ended. %.1f sec, %lld frames sent, avg %.1f FPS\n",
            sessionSec, (long long)totalSent,
            sessionSec > 0.0 ? totalSent / sessionSec : 0.0);

    capturer.Cleanup();
    sender.Cleanup();
    return 0;
}
