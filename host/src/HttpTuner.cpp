// ─── HttpTuner.cpp ────────────────────────────────────────────
// Embedded HTTP server for real-time aim parameter tuning.
// Access from any device on the LAN at http://<host-ip>:9999

#include "HttpTuner.h"

#include "httplib.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  JSON helpers (no library — hand-rolled for our tiny payloads)
// ═══════════════════════════════════════════════════════════════

static std::string jsonEscape(double v) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}
static std::string jsonStr(const char* key, double val) {
    return std::string("\"") + key + "\":" + jsonEscape(val);
}
static std::string jsonStr(const char* key, int val) {
    return std::string("\"") + key + "\":" + std::to_string(val);
}
static std::string jsonStr(const char* key, bool val) {
    return std::string("\"") + key + "\":" + (val ? "true" : "false");
}
static std::string jsonStr(const char* key, uint64_t val) {
    return std::string("\"") + key + "\":" + std::to_string(val);
}

// Simple float parser (std::stof would work but let's be explicit)
static float parseFloat(const std::string& s) {
    return static_cast<float>(std::atof(s.c_str()));
}

// Parse a float value from JSON like: "smoothFactor":0.15
static bool extractFloat(const std::string& body, const char* key, float& out) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    // skip whitespace
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
    auto end = pos;
    while (end < body.size() && (body[end] == '-' || body[end] == '.' ||
           (body[end] >= '0' && body[end] <= '9'))) end++;
    if (end == pos) return false;
    out = parseFloat(body.substr(pos, end - pos));
    return true;
}
static bool extractBool(const std::string& body, const char* key, bool& out) {
    std::string search = std::string("\"") + key + "\":";
    auto pos = body.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
    if (body.compare(pos, 4, "true") == 0) { out = true; return true; }
    if (body.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

HttpTuner::~HttpTuner() {
    Stop();
}

bool HttpTuner::Start(uint16_t port) {
    if (m_state.running) Stop();

    m_state.serverPort = port;
    m_state.running = true;
    m_thread = std::thread(&HttpTuner::ServerThread, this);

    // Brief wait for server to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fprintf(stderr, "[HttpTuner] Control panel ready at http://localhost:%u\n", port);
    fprintf(stderr, "[HttpTuner] From other devices: http://<host-ip>:%u\n", port);
    return true;
}

void HttpTuner::Stop() {
    if (m_state.running) {
        m_state.running = false;
        // httplib's stop() is called via the server's destructor-like behavior
        // when we join the thread. We signal via running=false
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  Server thread
// ═══════════════════════════════════════════════════════════════

void HttpTuner::ServerThread() {
    httplib::Server svr;

    // ── GET / — serve control panel from disk ────────────
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream f("web/index.html");
        if (f) {
            std::string html((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
            res.set_content(html, "text/html; charset=utf-8");
        } else {
            res.set_content("web/index.html not found. "
                            "Place it next to the exe or in the working directory.",
                            "text/plain");
        }
    });

    // ── GET /api/state — return JSON snapshot ──────────
    // Capture 'this' via the TuningState pointer that we
    // pass through httplib's user data mechanism...
    // Actually, let's use a lambda capture directly.
    // httplib handlers are synchronous, so we can safely
    // lock the mutex inside the handler.
    //
    // Problem: we need 'this' in the handler. Use a raw pointer.
    auto* pState = &m_state;

    svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_mutex);

        std::string json = "{";
        // Config
        json += "\"config\":{";
        json += jsonStr("Kp",            m_state.config.Kp) + ",";
        json += jsonStr("Kd",            m_state.config.Kd) + ",";
        json += jsonStr("aimRange",      (int)m_state.config.aimRange) + ",";
        json += jsonStr("minConfidence",       m_state.config.minConfidence) + ",";
        json += jsonStr("deltaHeadConfidence", m_state.config.deltaHeadConfidence) + ",";
        json += jsonStr("aimPoint",            m_state.config.aimPoint) + ",";
        json += jsonStr("headOffset",    m_state.config.headOffset) + ",";
        json += jsonStr("gameW",        (int)m_state.config.gameW) + ",";
        json += jsonStr("gameH",        (int)m_state.config.gameH) + ",";
        json += jsonStr("nativeW",      (int)m_state.config.nativeW) + ",";
        json += jsonStr("nativeH",      (int)m_state.config.nativeH) + ",";
        json += jsonStr("modelId",      (int)m_state.config.modelId) + ",";
        json += jsonStr("aimEnabled",    m_state.aimEnabled);
        json += "},";
        // Stats
        json += jsonStr("sendFps",    m_state.sendFps) + ",";
        json += jsonStr("captureFps", m_state.captureFps) + ",";
        json += jsonStr("pipelineMs", m_state.pipelineMs) + ",";
        json += jsonStr("compressMs", m_state.compressMs) + ",";
        json += jsonStr("freshFrames", m_state.freshFrames) + ",";
        json += jsonStr("cacheFrames", m_state.cacheFrames) + ",";
        json += jsonStr("totalSent",  m_state.totalSent) + ",";
        // Target
        json += "\"target\":{";
        json += jsonStr("active",     m_state.target.active) + ",";
        json += jsonStr("screenX",    m_state.target.screenX) + ",";
        json += jsonStr("screenY",    m_state.target.screenY) + ",";
        json += jsonStr("confidence", m_state.target.confidence) + ",";
        json += jsonStr("distance",   m_state.target.distance) + ",";
        json += jsonStr("classId",    m_state.target.classId);
        json += "}";
        json += "}";

        res.set_content(json, "application/json");
    });

    // ── POST /api/config — update parameters ───────────
    svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto& body = req.body;

        float f;
        bool  b;
        if (extractFloat(body, "Kp", f))            m_state.config.Kp            = f;
        if (extractFloat(body, "Kd", f))            m_state.config.Kd            = f;
        if (extractFloat(body, "aimRange", f))      m_state.config.aimRange      = f;
        if (extractFloat(body, "minConfidence", f))       m_state.config.minConfidence       = f;
        if (extractFloat(body, "deltaHeadConfidence", f)) m_state.config.deltaHeadConfidence = f;
        if (extractFloat(body, "headOffset", f))          m_state.config.headOffset          = f;
        if (extractFloat(body, "gameW", f))         m_state.config.gameW         = (int)f;
        if (extractFloat(body, "gameH", f))         m_state.config.gameH         = (int)f;
        if (extractFloat(body, "modelId", f))       m_state.config.modelId       = (uint8_t)f;
        if (extractFloat(body, "aimPoint", f))      m_state.config.aimPoint      = (int)f;
        if (extractBool(body, "aimEnabled", b))     m_state.aimEnabled           = b;

        res.set_content("{\"ok\":true}", "application/json");
    });

    // ── Bind and serve ─────────────────────────────────
    svr.set_keep_alive_max_count(1);
    svr.listen("0.0.0.0", m_state.serverPort);
}

// ═══════════════════════════════════════════════════════════════
//  Thread-safe accessors
// ═══════════════════════════════════════════════════════════════

AimConfig HttpTuner::GetConfig() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.config;
}

bool HttpTuner::IsAimEnabled() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_state.aimEnabled;
}

void HttpTuner::UpdateStats(double sendFps, double captureFps,
                             double pipelineMs, double compressMs,
                             int fresh, int cache, uint64_t totalSent) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.sendFps     = sendFps;
    m_state.captureFps  = captureFps;
    m_state.pipelineMs  = pipelineMs;
    m_state.compressMs  = compressMs;
    m_state.freshFrames = fresh;
    m_state.cacheFrames = cache;
    m_state.totalSent   = totalSent;
}

void HttpTuner::UpdateTarget(float screenX, float screenY,
                              float confidence, float distance, int classId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state.target.active     = true;
    m_state.target.screenX    = screenX;
    m_state.target.screenY    = screenY;
    m_state.target.confidence = confidence;
    m_state.target.distance   = distance;
    m_state.target.classId    = classId;
}

} // namespace SynapseX
