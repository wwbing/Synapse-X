#pragma once

// ── Mouse control via ddll64.dll ──────────────────────────────
// Loads the DLL at runtime, calls MoveR for relative mouse movement.
// Used for aim-assist: when Client returns detections, the Host
// smoothly moves the crosshair toward the best target.
//
// DLL API:
//   int  OpenDevice()                          — init mouse device
//   void MoveR(int dx, int dy)                 — relative move (pixels)

#include <windows.h>
#include <cstdint>
#include <cmath>

namespace SynapseX {

// ── Aim configuration (tune per game) ────────────────────────
struct AimConfig {
    float smoothFactor  = 0.15f;   // fraction of distance to move per frame (0-1)
    float aimRange      = 500.0f;  // max px from screen center to engage
    float sensitivity   = 1.0f;    // game sensitivity multiplier
    float minConfidence = 0.25f;   // ignore detections below this confidence
};

class MouseController {
public:
    MouseController() = default;
    ~MouseController();

    MouseController(const MouseController&) = delete;
    MouseController& operator=(const MouseController&) = delete;

    // Load ddll64.dll from `dllPath` and call OpenDevice.
    // dllPath: relative to exe dir, or absolute path.
    // Returns true on success.
    bool Load(const char* dllPath);

    // Release DLL. Called automatically by destructor.
    void Unload();

    bool IsLoaded() const { return m_loaded; }

    // ── Low-level move ──────────────────────────────────────

    // Move the mouse by (dx, dy) pixels relative to current position.
    // dx > 0 = right, dy > 0 = down.
    void MoveRelative(int dx, int dy);

    // ── Aim-assist (high-level) ─────────────────────────────

    // Given screen-space detection coordinates and screen dimensions,
    // calculate and execute a smooth relative mouse move toward
    // the target center. Uses AimConfig for smoothing parameters.
    //
    // Returns true if a move was executed (target in range, above confidence).
    bool AimAtTarget(float targetScreenX, float targetScreenY,
                     float confidence,
                     int screenW, int screenH,
                     const AimConfig& cfg = AimConfig{});

    // ── Aim tuning ──────────────────────────────────────────
    void SetConfig(const AimConfig& cfg) { m_config = cfg; }
    const AimConfig& GetConfig() const { return m_config; }

private:
    HINSTANCE m_dll    = nullptr;
    bool      m_loaded = false;

    using MoveRFn = void (*)(int, int);
    MoveRFn m_moveR = nullptr;

    AimConfig m_config;
};

} // namespace SynapseX
