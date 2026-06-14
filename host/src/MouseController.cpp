// ─── MouseController.cpp ─────────────────────────────────────
// Runtime DLL loader for ddll64.dll.
// Aim-assist: smooth relative movement toward the best detection.

#include "MouseController.h"

#include <algorithm>
#include <cstdio>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  DLL lifecycle
// ═══════════════════════════════════════════════════════════════

MouseController::~MouseController() {
    Unload();
}

bool MouseController::Load(const char* dllPath) {
    if (m_loaded) Unload();

    m_dll = LoadLibraryA(dllPath);
    if (!m_dll) {
        fprintf(stderr, "[MouseCtrl] LoadLibraryA('%s') FAILED (err=%lu). "
                "Is ddll64.dll next to the exe?\n",
                dllPath, static_cast<unsigned long>(GetLastError()));
        return false;
    }

    // OpenDevice — must be called once before any movement
    using OpenDeviceFn = int (*)();
    auto openDev = reinterpret_cast<OpenDeviceFn>(
        GetProcAddress(m_dll, "OpenDevice"));
    if (!openDev || openDev() == 0) {
        fprintf(stderr, "[MouseCtrl] OpenDevice FAILED. "
                "Try running as Administrator.\n");
        FreeLibrary(m_dll);
        m_dll = nullptr;
        return false;
    }

    // MoveR — relative mouse movement
    m_moveR = reinterpret_cast<MoveRFn>(
        GetProcAddress(m_dll, "MoveR"));
    if (!m_moveR) {
        fprintf(stderr, "[MouseCtrl] GetProcAddress('MoveR') FAILED\n");
        FreeLibrary(m_dll);
        m_dll = nullptr;
        return false;
    }

    m_loaded = true;
    fprintf(stderr, "[MouseCtrl] Ready -- ddll64.dll loaded, MoveR available\n");
    return true;
}

void MouseController::Unload() {
    if (m_dll) {
        FreeLibrary(m_dll);
        m_dll    = nullptr;
        m_moveR  = nullptr;
        m_loaded = false;
    }
}

// ═══════════════════════════════════════════════════════════════
//  Low-level move
// ═══════════════════════════════════════════════════════════════

void MouseController::MoveRelative(int dx, int dy) {
    if (m_moveR) {
        m_moveR(dx, dy);
    }
}

// ═══════════════════════════════════════════════════════════════
//  Aim-assist
// ═══════════════════════════════════════════════════════════════

bool MouseController::AimAtTarget(float targetScreenX, float targetScreenY,
                                   float confidence,
                                   int screenW, int screenH,
                                   const AimConfig& cfg) {
    if (!m_loaded) return false;
    if (confidence < cfg.minConfidence) return false;

    // ── Vector from screen center to target center ─────────
    float screenCx = static_cast<float>(screenW) * 0.5f;
    float screenCy = static_cast<float>(screenH) * 0.5f;

    float dx = targetScreenX - screenCx;
    float dy = targetScreenY - screenCy;

    // ── Distance check ────────────────────────────────────
    float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > cfg.aimRange) return false;
    if (dist < 1.0f) return false;  // already on target

    // ── Smooth movement ───────────────────────────────────
    // Move a fraction of the remaining distance each frame.
    // This creates an exponential decay toward the target:
    //   far away → fast movement
    //   close     → fine adjustment
    float moveX = dx * cfg.smoothFactor * cfg.sensitivity;
    float moveY = dy * cfg.smoothFactor * cfg.sensitivity;

    // Clamp: minimum 1px to avoid getting stuck
    if (std::abs(moveX) < 1.0f && std::abs(moveY) < 1.0f) {
        moveX = (dx > 0.0f) ? 1.0f : (dx < 0.0f) ? -1.0f : 0.0f;
        moveY = (dy > 0.0f) ? 1.0f : (dy < 0.0f) ? -1.0f : 0.0f;
    }

    MoveRelative(static_cast<int>(moveX), static_cast<int>(moveY));
    return true;
}

} // namespace SynapseX
