#pragma once

// ── Web-based tuning panel ────────────────────────────────────
// Runs a minimal HTTP server on localhost:9999.
// Any device on the LAN (phone, tablet, laptop) can open
// http://192.168.100.1:9999 and adjust aim parameters in real time.
//
// API:
//   GET  /            → embedded HTML control panel
//   GET  /api/state   → JSON: current config + pipeline stats
//   POST /api/config  → JSON body: updated AimConfig fields
//
// Dependencies: cpp-httplib (single header, thirdparty/)

#include "MouseController.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace SynapseX {

struct TuningState {
    // ── Aim config (writable via web UI) ────────────────
    AimConfig config;
    bool      aimEnabled = true;

    // ── Pipeline stats (written by main loop, read by web UI)
    double sendFps       = 0.0;
    double captureFps    = 0.0;
    double pipelineMs    = 0.0;   // total pipeline latency
    double compressMs    = 0.0;
    int    freshFrames   = 0;
    int    cacheFrames   = 0;
    uint64_t totalSent   = 0;

    // ── Latest detection info ───────────────────────────
    struct TargetInfo {
        bool   active      = false;
        float  screenX     = 0.0f;
        float  screenY     = 0.0f;
        float  confidence  = 0.0f;
        float  distance    = 0.0f;
        int    classId     = 0;
    };
    TargetInfo target;

    // ── Oscilloscope ring buffer (for web chart) ─────────
    static constexpr int kScopeSize = 256;
    struct ScopePoint { float rawDx; float emaDx; float output; float residual; };
    ScopePoint scopeBuf[kScopeSize] = {};
    int        scopeWriteIdx = 0;
    int        scopeCount    = 0;

    // ── Server info ─────────────────────────────────────
    int     serverPort  = 9999;
    bool    running     = false;
};

class HttpTuner {
public:
    HttpTuner() = default;
    ~HttpTuner();

    HttpTuner(const HttpTuner&) = delete;
    HttpTuner& operator=(const HttpTuner&) = delete;

    // Start HTTP server in a background thread.
    // Port: default 9999. Only binds to localhost for security.
    bool Start(uint16_t port = 9999);

    // Stop server and join thread.
    void Stop();

    bool IsRunning() const { return m_state.running; }

    // ── Thread-safe access from main loop ────────────────

    // Read current config (for aim-assist)
    AimConfig GetConfig() const;
    bool      IsAimEnabled() const;

    // Update pipeline stats (call once per second)
    void UpdateStats(double sendFps, double captureFps,
                     double pipelineMs, double compressMs,
                     int fresh, int cache, uint64_t totalSent);

    // Update latest target info
    void UpdateTarget(float screenX, float screenY,
                      float confidence, float distance, int classId);

    // Push one oscilloscope data point (called from main loop each aim tick)
    void UpdateScope(float rawDx, float emaDx, float output, float residual);

private:
    void ServerThread();

    TuningState m_state;
    mutable std::mutex m_mutex;
    std::thread m_thread;
};

} // namespace SynapseX
