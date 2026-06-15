// ─── Synapse-X Host -- Production main loop ─────────────────
//
// Fixed 170 Hz pipeline:
//   DXGI capture (try every tick)
//     -> new frame?  LZ4 compress + update cache
//     -> no change?  re-send cached compressed frame
//   UDP fragment & send (every tick)
//   UDP reply receive (drain after each send)
//
// Usage:
//   SynapseX_Host.exe [target_ip] [port] [roi_w] [roi_h]
//   Defaults: 192.168.100.2  8888   640     640

#include "DxgiCapturer.h"
#include "Lz4Compressor.h"
#include "UdpSender.h"
#include "UdpReplyReceiver.h"
#include "MouseController.h"
#include "HttpTuner.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>

#include <windows.h>    // timeBeginPeriod / timeEndPeriod
#include <mmsystem.h>

static std::atomic<bool> g_running{true};

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

struct PerfStats {
    int    captured    = 0;    // new frames from DXGI this window
    int    sent        = 0;    // total UDP sends this window
    double sumCapture  = 0.0;  // only measured on new frames
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

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    const char* targetIp   = (argc > 1) ? argv[1] : "192.168.100.2";
    uint16_t    targetPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8888;
    int         roiW       = (argc > 3) ? std::atoi(argv[3]) : 640;
    int         roiH       = (argc > 4) ? std::atoi(argv[4]) : 640;

    if (roiW < 64 || roiH < 64 || roiW > 4096 || roiH > 4096) {
        fprintf(stderr, "[FATAL] Invalid ROI: %dx%d (min 64, max 4096)\n", roiW, roiH);
        return 1;
    }

    int rawSize = roiW * roiH * 4;

    // ── Fixed 170 Hz cadence ──────────────────────────────
    constexpr double kTargetFps  = 170.0;
    constexpr double kTargetMs   = 1000.0 / kTargetFps;   // ~5.882 ms
    const auto       kInterval   = std::chrono::duration<double, std::milli>(kTargetMs);

    // Boost Windows timer resolution.
    // Default is 15.6 ms — way too coarse for 5.88 ms ticks.
    timeBeginPeriod(1);

    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X Host -- Fixed %.0f Hz Pipeline\n", kTargetFps);
    fprintf(stderr, "  Target: %s:%u\n", targetIp, targetPort);
    fprintf(stderr, "  ROI:    %dx%d  (%.2f MB raw)\n",
            roiW, roiH, rawSize / (1024.0 * 1024.0));
    fprintf(stderr, "  Budget: %.2f ms per frame\n", kTargetMs);
    fprintf(stderr, "============================================\n\n");

    // ── Stage 0: Init all modules ────────────────────────
    SynapseX::DxgiCapturer capturer;
    if (!capturer.Initialize(roiW, roiH)) {
        fprintf(stderr, "[FATAL] DxgiCapturer init FAILED.\n");
        return 1;
    }

    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        fprintf(stderr, "[FATAL] Lz4Compressor init FAILED.\n");
        return 1;
    }

    SynapseX::UdpSender sender;
    if (!sender.Initialize(targetIp, targetPort)) {
        fprintf(stderr, "[FATAL] UdpSender init FAILED.\n");
        return 1;
    }

    // UDP reply receiver (Client -> Host, port 8889)
    SynapseX::UdpReplyReceiver replyReceiver;
    if (!replyReceiver.Initialize(8889)) {
        fprintf(stderr, "[FATAL] UdpReplyReceiver init FAILED.\n");
        return 1;
    }
    replyReceiver.SetRoiParams(roiW, roiH,
                                capturer.GetOutputWidth(),
                                capturer.GetOutputHeight());

    // Mouse controller (ddll64.dll for aim-assist)
    SynapseX::MouseController mouse;

    if (!mouse.Load("ddll64.dll")) {
        fprintf(stderr, "[WARN] MouseController init FAILED — "
                "aim-assist disabled. Is ddll64.dll next to the exe?\n");
    }

    // Web tuning panel (runs in background thread)
    SynapseX::HttpTuner tuner;
    if (!tuner.Start(9999)) {
        fprintf(stderr, "[WARN] HttpTuner init FAILED — web panel unavailable.\n");
    }

    int screenW = capturer.GetOutputWidth();
    int screenH = capturer.GetOutputHeight();

    fprintf(stderr, "[INFO] All modules initialized. Starting main loop...\n");
    fprintf(stderr, "[INFO] If game capture fails, try Borderless Windowed mode.\n");
    fprintf(stderr, "[INFO] Press Ctrl+C to stop.\n\n");

    // ── Diagnostic counters ──────────────────────────────
    int      zeroFrameCount = 0;
    bool     warnedProtected = false;
    bool     warnedZero = false;

    // ── Main loop state ──────────────────────────────────
    std::vector<uint8_t> rawBuffer;
    rawBuffer.reserve(rawSize);

    std::vector<uint8_t> compressedBuffer;
    compressedBuffer.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // Cached compressed frame — re-sent when desktop is idle.
    std::vector<uint8_t> cachedCompressed;
    cachedCompressed.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));
    bool hasCachedFrame = false;

    uint32_t    frameId       = 0;
    PerfStats   stats;
    TimePoint   windowStart   = Clock::now();
    int64_t     totalSent     = 0;
    TimePoint   sessionStart  = Clock::now();

    const uint16_t roiW16 = static_cast<uint16_t>(roiW);
    const uint16_t roiH16 = static_cast<uint16_t>(roiH);

    auto nextTick = Clock::now();

    // ═══════════════════════════════════════════════════════
    //  MAIN LOOP — Fixed 170 Hz
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── Stage 1: Capture (try every tick) ─────────────
        auto t0 = Clock::now();
        bool gotFrame = capturer.CaptureFrame(rawBuffer);
        auto t1 = Clock::now();

        if (gotFrame) {
            // ── Diagnostic: detect zero / protected frames ──
            const auto& fi = capturer.GetLastFrameInfo();
            if (fi.ProtectedContentMaskedOut && !warnedProtected) {
                fprintf(stderr,
                    "[DIAG] ProtectedContentMaskedOut detected! "
                    "Game DRM or anti-cheat is blocking capture.\n"
                    "[DIAG] Try running the game in Borderless Windowed mode.\n");
                warnedProtected = true;
            }

            bool allZero = true;
            for (size_t k = 0; k < rawBuffer.size(); ++k) {
                if (rawBuffer[k] != 0) { allZero = false; break; }
            }
            if (allZero) {
                zeroFrameCount++;
                if (!warnedZero && zeroFrameCount > 10) {
                    fprintf(stderr,
                        "[DIAG] %d consecutive all-zero frames! "
                        "Screen=(%d,%d) ROI=(%d,%d) Protected=%u\n"
                        "[DIAG] Game is likely in Exclusive Fullscreen — "
                        "switch to Borderless Windowed.\n",
                        zeroFrameCount, screenW, screenH, roiW, roiH,
                        fi.ProtectedContentMaskedOut);
                    warnedZero = true;
                }
            } else {
                zeroFrameCount = 0;
                warnedZero = false;
            }

            // ── Stage 2: Compress (only on new content) ───
            auto t2a = Clock::now();
            bool ok = compressor.Compress(rawBuffer.data(),
                                           static_cast<int>(rawBuffer.size()),
                                           compressedBuffer);
            auto t2b = Clock::now();

            if (ok) {
                // Update cache for future re-sends
                cachedCompressed = compressedBuffer;
                hasCachedFrame   = true;

                stats.captured++;
                stats.sumCapture  += ToMs(t1  - t0);
                stats.sumCompress += ToMs(t2b - t2a);
            }
        }

        // ── Stage 3: Send (ALWAYS — reuses cache if idle) ──
        if (hasCachedFrame) {
            auto t3a = Clock::now();
            bool sent = sender.SendCompressedFrame(
                cachedCompressed.data(),
                static_cast<uint32_t>(cachedCompressed.size()),
                frameId,
                roiW16, roiH16);
            auto t3b = Clock::now();

            if (sent) totalSent++;

            stats.sent++;
            stats.sumSend += ToMs(t3b - t3a);
            frameId++;
        }

        // ── Stage 4: Receive inference replies ─────────────
        {
            std::vector<SynapseX::Detection> detections;
            uint32_t replyFrameId = 0;
            if (replyReceiver.ReceiveReplies(detections, replyFrameId)) {
                if (!detections.empty()) {
                    // Pick best target: highest-confidence enemy (classId=0)
                    // closest to screen center
                    const SynapseX::Detection* best = nullptr;
                    float bestDist = 1e9f;
                    float screenCx = static_cast<float>(screenW) * 0.5f;
                    float screenCy = static_cast<float>(screenH) * 0.5f;

                    for (const auto& d : detections) {
                        if (d.classId != 0) continue;  // enemy only
                        float cx = (d.x1 + d.x2) * 0.5f;
                        float cy = (d.y1 + d.y2) * 0.5f;
                        float dist = std::sqrt(
                            (cx - screenCx) * (cx - screenCx) +
                            (cy - screenCy) * (cy - screenCy));
                        // Prefer higher confidence; break ties with distance
                        if (!best || d.confidence > best->confidence ||
                            (d.confidence == best->confidence && dist < bestDist)) {
                            best = &d;
                            bestDist = dist;
                        }
                    }

                    if (best) {
                        // Read config (may be changed via web UI)
                        auto aimCfg = tuner.GetConfig();

                        // Compute aim point based on config
                        float bboxH = best->y2 - best->y1;
                        float targetCx = (best->x1 + best->x2) * 0.5f;
                        float targetCy;
                        if (aimCfg.aimPoint == 1) {
                            targetCy = best->y1 + bboxH * aimCfg.headOffset;
                        } else {
                            targetCy = (best->y1 + best->y2) * 0.5f;
                        }

                        // Compute error vector from screen center
                        float dx = targetCx - static_cast<float>(screenW) * 0.5f;
                        float dy = targetCy - static_cast<float>(screenH) * 0.5f;

                        // Update web panel target info
                        tuner.UpdateTarget(targetCx, targetCy,
                                           best->confidence, bestDist,
                                           static_cast<int>(best->classId));

                        mouse.SetConfig(aimCfg);

                        if (tuner.IsAimEnabled() &&
                            mouse.AimAtTarget(dx, dy,
                                              best->confidence,
                                              screenW, screenH, aimCfg)) {
                            // Throttled log: ~every 180ms
                            static int aimCount = 0;
                            if (++aimCount % 30 == 1) {
                                fprintf(stderr,
                                    "[Aim] tgt=%.0f,%.0f conf=%.2f dist=%.0f\n",
                                    static_cast<double>(targetCx),
                                    static_cast<double>(targetCy),
                                    static_cast<double>(best->confidence),
                                    static_cast<double>(bestDist));
                            }
                        }
                    }
                }
            }
        }

        // ── Per-second stats report ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            double sendFps    = stats.sent / elapsed;
            double captureFps = stats.captured / elapsed;
            double avgCapture = stats.captured > 0 ? stats.sumCapture  / stats.captured : 0.0;
            double avgCompress= stats.captured > 0 ? stats.sumCompress / stats.captured : 0.0;
            double avgSend    = stats.sent     > 0 ? stats.sumSend     / stats.sent     : 0.0;

            double pipelineTotal = avgCapture + avgCompress + avgSend;

            fprintf(stderr,
                "---- per-second stats --------------------------------\n"
                "  Send FPS: %6.1f  |  capture FPS: %6.1f  |  "
                "fresh: %d  cache: %d  |  total: %lld\n"
                "  capture: %8.3f ms  |  compress: %8.3f ms  |  "
                "send: %8.3f ms\n"
                "  budget: %5.2f ms @%.0f Hz\n",
                sendFps, captureFps,
                stats.captured, stats.sent - stats.captured,
                (long long)totalSent,
                avgCapture, avgCompress, avgSend,
                kTargetMs, kTargetFps);

            // Push to web tuning panel
            tuner.UpdateStats(sendFps, captureFps,
                              pipelineTotal, avgCompress,
                              stats.captured, stats.sent - stats.captured,
                              static_cast<uint64_t>(totalSent));

            stats.reset();
            windowStart = Clock::now();
        }

        // ── Maintain 170 Hz cadence ──────────────────────
        nextTick += std::chrono::duration_cast<Clock::duration>(kInterval);
        auto now = Clock::now();
        if (nextTick > now) {
            std::this_thread::sleep_until(nextTick);
        } else {
            // Fell behind schedule — reset to avoid death-spiral
            nextTick = now;
        }
    }

    timeEndPeriod(1);

    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    fprintf(stderr, "\n[DONE] Session ended. %.1f sec, %lld frames sent, avg %.1f FPS\n",
            sessionSec, (long long)totalSent,
            sessionSec > 0.0 ? totalSent / sessionSec : 0.0);

    capturer.Cleanup();
    sender.Cleanup();
    replyReceiver.Cleanup();
    mouse.Unload();
    tuner.Stop();
    return 0;
}
