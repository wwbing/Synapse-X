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

// ── Intermediate aim-point (unified across all models) ──
struct AimPoint {
    float cx, cy;       // screen-space target coordinates
    int   priority;     // 1 = primary (real head / single-class), 2 = fallback (body-faked head)
    float distance;     // px from screen center
};

int main(int argc, char* argv[]) {
    // ── Parse arguments ──────────────────────────────────
    const char* targetIp   = (argc > 1) ? argv[1] : "192.168.100.2";
    uint16_t    targetPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8888;
    int         roiW       = (argc > 3) ? std::atoi(argv[3]) : 416;
    int         roiH       = (argc > 4) ? std::atoi(argv[4]) : 416;

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

    // Pin main thread to a fixed P-core to prevent OS scheduler
    // from bouncing it across cores — avoids L1/L2 cache thrashing.
    DWORD_PTR affinityMask = 1ULL << 2;  // Core 2 (adjust per CPU topology)
    if (!SetThreadAffinityMask(GetCurrentThread(), affinityMask)) {
        fprintf(stderr, "[WARN] SetThreadAffinityMask failed (err=%lu)\n",
                static_cast<unsigned long>(GetLastError()));
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    fprintf(stderr, "[INFO] Thread pinned to core 2, priority TIME_CRITICAL\n");

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

    // ── Spatial lock state (priority-aware) ────────────────
    constexpr float kKeepLockRadius = 80.0f;   // px — max dist to maintain lock
    constexpr int   kMaxLostFrames  = 5;       // frames before giving up

    bool  isLocked        = false;
    float lockedTargetX   = 0.0f;
    float lockedTargetY   = 0.0f;
    int   lostFrames      = 0;
    int   lockedPriority  = 0;  // 1=primary, 2=fallback

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
                roiW16, roiH16,
                tuner.GetConfig().modelId);
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
                    auto aimCfg = tuner.GetConfig();
                    uint8_t modelId = aimCfg.modelId;

                    static uint8_t lastModelId = 0xFF;
                    if (modelId != lastModelId) {
                        fprintf(stderr, "[Model] Switched to ID %u\n",
                                static_cast<unsigned>(modelId));
                        lastModelId = modelId;
                    }

                    // ── Data Normalizer: all models → unified AimPoint[] ──
                    float scrCx = static_cast<float>(screenW) * 0.5f;
                    float scrCy = static_cast<float>(screenH) * 0.5f;

                    std::vector<AimPoint> aimPoints;
                    aimPoints.reserve(detections.size() * 2); // Delta may double

                    for (const auto& d : detections) {
                        float bw = d.x2 - d.x1;
                        float bh = d.y2 - d.y1;
                        float bcx = (d.x1 + d.x2) * 0.5f;
                        float bcyCenter = (d.y1 + d.y2) * 0.5f;
                        float bcyHead   = d.y1 + bh * aimCfg.headOffset;

                        AimPoint ap;
                        ap.cx = bcx;

                        switch (modelId) {
                        case 0: // Apex      — 1-class: classId 0 = enemy
                        case 3: // OW2       — 1-class: classId 0 = enemy
                            if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;

                        case 1: // Delta     — 2-class: 0=body, 1=head
                            if (aimCfg.aimPoint == 1) {
                                // Head mode: real head bypasses minConfidence (head bboxes are small, low conf is normal)
                                if (d.classId == 1) {
                                    ap.cy       = bcyCenter; // real head center
                                    ap.priority = 1;
                                } else if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                    ap.cy       = bcyHead;   // body → faked head (filtered)
                                    ap.priority = 2;
                                } else { break; }
                            } else {
                                // Body mode: only body, filtered by confidence
                                if (d.classId != 0 || d.confidence < aimCfg.minConfidence) break;
                                ap.cy       = bcyCenter;
                                ap.priority = 1;
                            }
                            ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                            aimPoints.push_back(ap);
                            break;

                        case 2: // BF6       — 2-class: 0=enemy, 1=teammate (DROP)
                            if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;

                        default:
                            if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;
                        }
                    }

                    if (!aimPoints.empty()) {
                        const AimPoint* best = nullptr;
                        float bestDist = 1e9f;

                        if (isLocked) {
                            // ── Phase A: Maintain Lock ──────────────
                            const AimPoint* bestPri1 = nullptr;
                            const AimPoint* bestPri2 = nullptr;
                            float dPri1 = 1e9f, dPri2 = 1e9f;

                            for (const auto& ap : aimPoints) {
                                float d = std::sqrt(
                                    (ap.cx - lockedTargetX) * (ap.cx - lockedTargetX) +
                                    (ap.cy - lockedTargetY) * (ap.cy - lockedTargetY));
                                if (d < kKeepLockRadius) {
                                    if (ap.priority == 1 && d < dPri1) {
                                        dPri1 = d; bestPri1 = &ap;
                                    } else if (ap.priority == 2 && d < dPri2) {
                                        dPri2 = d; bestPri2 = &ap;
                                    }
                                }
                            }

                            if (bestPri1) {
                                best = bestPri1;
                                lockedPriority = 1;
                                lostFrames = 0;
                            } else if (bestPri2) {
                                best = bestPri2;
                                lockedPriority = 2;
                                lostFrames = 0;
                            } else {
                                lostFrames++;
                                if (lostFrames > kMaxLostFrames) {
                                    isLocked = false;
                                    lostFrames = 0;
                                    lockedPriority = 0;
                                }
                            }
                        } else {
                            // ── Phase B: Acquire Lock ───────────────
                            const AimPoint* bestPri1 = nullptr;
                            const AimPoint* bestPri2 = nullptr;
                            float dPri1 = 1e9f, dPri2 = 1e9f;

                            for (const auto& ap : aimPoints) {
                                if (ap.distance > aimCfg.aimRange) continue;
                                if (ap.priority == 1 && ap.distance < dPri1) {
                                    dPri1 = ap.distance; bestPri1 = &ap;
                                } else if (ap.priority == 2 && ap.distance < dPri2) {
                                    dPri2 = ap.distance; bestPri2 = &ap;
                                }
                            }

                            if (bestPri1) {
                                best = bestPri1;
                                lockedPriority = 1;
                                isLocked = true;
                                lostFrames = 0;
                            } else if (bestPri2) {
                                best = bestPri2;
                                lockedPriority = 2;
                                isLocked = true;
                                lostFrames = 0;
                            }
                        }

                        if (best) {
                            lockedTargetX = best->cx;
                            lockedTargetY = best->cy;

                            float autoScaleX = (screenW > 0)
                                ? static_cast<float>(aimCfg.gameW) / static_cast<float>(screenW) : 1.0f;
                            float autoScaleY = (screenH > 0)
                                ? static_cast<float>(aimCfg.gameH) / static_cast<float>(screenH) : 1.0f;

                            float dx = (best->cx - scrCx) * autoScaleX;
                            float dy = (best->cy - scrCy) * autoScaleY;

                            bestDist = best->distance;

                            tuner.UpdateTarget(best->cx, best->cy,
                                               1.0f, bestDist, 0);

                            mouse.SetConfig(aimCfg);

                            if (tuner.IsAimEnabled() &&
                                mouse.AimAtTarget(dx, dy, 1.0f,
                                                  screenW, screenH, aimCfg)) {
                                static int aimCount = 0;
                                if (++aimCount % 30 == 1) {
                                    fprintf(stderr,
                                        "[Aim] tgt=%.0f,%.0f pri=%d dist=%.0f\n",
                                        static_cast<double>(best->cx),
                                        static_cast<double>(best->cy),
                                        best->priority,
                                        static_cast<double>(bestDist));
                                }
                            }
                        }
                    }
                }
            }
        }        // ── Per-second stats report ──────────────────────
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
