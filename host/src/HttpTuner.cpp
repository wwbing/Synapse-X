// ─── HttpTuner.cpp ────────────────────────────────────────────
// Embedded HTTP server for real-time aim parameter tuning.
// Access from any device on the LAN at http://<host-ip>:9999

#include "HttpTuner.h"

#include "httplib.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  Embedded control panel HTML
// ═══════════════════════════════════════════════════════════════

static const char kHtmlPage[] = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Synapse-X Control Panel</title>
<style>
  :root {
    --bg: #0d1117; --card: #161b22; --border: #30363d;
    --accent: #58a6ff; --text: #c9d1d9; --dim: #8b949e;
    --green: #3fb950; --red: #f85149; --orange: #d2991d;
  }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { background:var(--bg); color:var(--text); font-family:'Segoe UI',sans-serif;
         display:flex; justify-content:center; padding:20px; }
  .panel { max-width:520px; width:100%; }
  h1 { font-size:20px; margin-bottom:4px; color:var(--accent); }
  .sub { color:var(--dim); font-size:13px; margin-bottom:20px; }
  .card { background:var(--card); border:1px solid var(--border);
          border-radius:8px; padding:16px; margin-bottom:16px; }
  .card h2 { font-size:14px; color:var(--dim); text-transform:uppercase;
             letter-spacing:1px; margin-bottom:12px; }
  label { display:flex; justify-content:space-between; align-items:center;
          font-size:14px; margin-bottom:6px; }
  label span.val { color:var(--accent); font-weight:600; min-width:42px; text-align:right; }
  input[type=range] { -webkit-appearance:none; width:100%; height:6px;
    border-radius:3px; background:var(--border); outline:none; margin-bottom:12px; }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance:none; width:18px; height:18px;
    border-radius:50%; background:var(--accent); cursor:pointer; }
  .toggle { display:flex; align-items:center; gap:10px; margin:12px 0; }
  .toggle input { display:none; }
  .toggle label.tgl { width:44px; height:24px; background:var(--border);
    border-radius:12px; cursor:pointer; position:relative; transition:background .2s; }
  .toggle label.tgl::after { content:''; position:absolute; top:2px; left:2px;
    width:20px; height:20px; border-radius:50%; background:#fff; transition:left .2s; }
  .toggle input:checked + label.tgl { background:var(--green); }
  .toggle input:checked + label.tgl::after { left:22px; }
  .stat-row { display:grid; grid-template-columns:1fr 1fr; gap:8px; font-size:13px; }
  .stat-item { display:flex; justify-content:space-between; padding:4px 8px;
               background:#0d1117; border-radius:4px; }
  .stat-val { color:var(--accent); font-weight:600; }
  .stat-val.good { color:var(--green); }
  .stat-val.warn { color:var(--orange); }
  .target-box { font-size:13px; padding:8px; background:#0d1117; border-radius:4px;
                margin-top:8px; display:none; }
  .target-box.active { display:block; border-left:3px solid var(--red); }
  .target-box .cls { color:var(--red); font-weight:600; }
  .footer { text-align:center; color:var(--dim); font-size:11px; margin-top:8px; }
</style>
</head>
<body>
<div class="panel">
  <h1>Synapse-X Control Panel</h1>
  <div class="sub">Real-time parameter tuning &mdash; changes apply instantly</div>

  <!-- Aim Config -->
  <div class="card">
    <h2>Aim Parameters</h2>
    <label>Kp (Proportional) <span class="val" id="vKp">0.40</span></label>
    <input type="range" id="Kp" min="0.05" max="1.50" step="0.01" value="0.40">

    <label>Kd (Damping) <span class="val" id="vKd">0.05</span></label>
    <input type="range" id="Kd" min="0.00" max="0.50" step="0.01" value="0.05">

    <label>Aim Range <span class="val" id="vaimRange">500</span></label>
    <input type="range" id="aimRange" min="50" max="1000" step="10" value="500">

    <label>Min Confidence <span class="val" id="vminConfidence">0.25</span></label>
    <input type="range" id="minConfidence" min="0.0" max="1.0" step="0.01" value="0.25">

    <label>Aim Point <span class="val" id="vaimPoint">Body</span></label>
    <select id="aimPoint" style="width:100%;padding:6px;background:var(--bg);color:var(--text);
      border:1px solid var(--border);border-radius:4px;margin-bottom:12px;font-size:14px;">
      <option value="0">Body (bbox center)</option>
      <option value="1">Head (top of bbox)</option>
    </select>

    <label>Head Offset <span class="val" id="vheadOffset">0.12</span></label>
    <input type="range" id="headOffset" min="0.05" max="0.25" step="0.01" value="0.12">

    <div class="toggle">
      <span>Aim Enabled</span>
      <input type="checkbox" id="aimEnabled" checked>
      <label class="tgl" for="aimEnabled"></label>
    </div>
  </div>

  <!-- Stats -->
  <div class="card">
    <h2>Pipeline Stats</h2>
    <div class="stat-row" id="stats"></div>
    <div class="target-box" id="targetBox">
      Target: <span class="cls" id="tCls">enemy</span>
      &nbsp; conf=<span id="tConf">0.84</span>
      &nbsp; dist=<span id="tDist">234</span>px
      &nbsp; pos=(<span id="tX">0</span>,<span id="tY">0</span>)
    </div>
  </div>

  <div class="footer">Synapse-X &bull; connected to <span id="hostInfo">localhost:9999</span></div>
</div>

<script>
const HOST = location.origin;

function $(id) { return document.getElementById(id); }

// ── Sliders → POST config ──────────────────────────────
['Kp','Kd','aimRange','minConfidence','headOffset'].forEach(id => {
  $(id).addEventListener('input', () => {
    let v = $(id).value;
    if (id==='aimRange') v = parseInt(v);
    if (id==='Kd') v = parseFloat(v).toFixed(2);
    $('v'+id).textContent = v;
    postConfig();
  });
});
$('aimEnabled').addEventListener('change', postConfig);
$('aimPoint').addEventListener('change', function() {
  $('vaimPoint').textContent = this.value==='1' ? 'Head' : 'Body';
  postConfig();
});

function getConfig() {
  return {
    Kp:            parseFloat($('Kp').value),
    Kd:            parseFloat($('Kd').value),
    aimRange:      parseInt($('aimRange').value),
    minConfidence: parseFloat($('minConfidence').value),
    aimPoint:      parseInt($('aimPoint').value),
    headOffset:    parseFloat($('headOffset').value),
    aimEnabled:    $('aimEnabled').checked
  };
}
function setConfig(cfg) {
  $('Kp').value            = cfg.Kp || 0.40;
  $('Kd').value            = (cfg.Kd != null) ? cfg.Kd : 0.05;
  $('aimRange').value      = cfg.aimRange;
  $('minConfidence').value = cfg.minConfidence;
  $('aimPoint').value      = cfg.aimPoint || 0;
  $('headOffset').value    = cfg.headOffset || 0.12;
  $('aimEnabled').checked  = cfg.aimEnabled;
  $('vKp').textContent            = (cfg.Kp||0.40).toFixed(2);
  $('vKd').textContent            = (cfg.Kd||0.05).toFixed(2);
  $('vaimRange').textContent      = cfg.aimRange;
  $('vminConfidence').textContent = cfg.minConfidence;
  $('vaimPoint').textContent      = (cfg.aimPoint===1) ? 'Head' : 'Body';
  $('vheadOffset').textContent    = (cfg.headOffset||0.12).toFixed(2);
}
function postConfig() {
  fetch(HOST+'/api/config', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body: JSON.stringify(getConfig())
  }).catch(()=>{});
}

// ── Poll stats ─────────────────────────────────────────
function poll() {
  fetch(HOST+'/api/state').then(r=>r.json()).then(s => {
    if (s.config && !document.initialConfigLoaded) {
      setConfig(s.config); document.initialConfigLoaded = true;
    }
    $('stats').innerHTML =
      '<div class="stat-item">Send FPS <span class="stat-val good">'+s.sendFps.toFixed(0)+'</span></div>'+
      '<div class="stat-item">Capture FPS <span class="stat-val">'+s.captureFps.toFixed(0)+'</span></div>'+
      '<div class="stat-item">Pipeline <span class="stat-val good">'+s.pipelineMs.toFixed(2)+' ms</span></div>'+
      '<div class="stat-item">Compress <span class="stat-val">'+s.compressMs.toFixed(2)+' ms</span></div>'+
      '<div class="stat-item">Fresh frames <span class="stat-val">'+s.freshFrames+'</span></div>'+
      '<div class="stat-item">Cache frames <span class="stat-val">'+s.cacheFrames+'</span></div>'+
      '<div class="stat-item">Total sent <span class="stat-val">'+s.totalSent+'</span></div>'+
      '<div class="stat-item">Uptime <span class="stat-val">'+(s.totalSent/170).toFixed(0)+'s</span></div>';
    if (s.target && s.target.active) {
      $('targetBox').className = 'target-box active';
      $('tCls').textContent = s.target.classId===0?'enemy':'teammate';
      $('tConf').textContent = s.target.confidence.toFixed(2);
      $('tDist').textContent = s.target.distance.toFixed(0);
      $('tX').textContent = s.target.screenX.toFixed(0);
      $('tY').textContent = s.target.screenY.toFixed(0);
    } else {
      $('targetBox').className = 'target-box';
    }
  }).catch(()=>{});
  setTimeout(poll, 500);
}
poll();
</script>
</body>
</html>)html";

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

    // ── GET / — serve control panel ────────────────────
    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(kHtmlPage, "text/html; charset=utf-8");
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
        json += jsonStr("minConfidence", m_state.config.minConfidence) + ",";
        json += jsonStr("aimPoint",      m_state.config.aimPoint) + ",";
        json += jsonStr("headOffset",    m_state.config.headOffset) + ",";
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
        if (extractFloat(body, "minConfidence", f)) m_state.config.minConfidence = f;
        if (extractFloat(body, "headOffset", f))    m_state.config.headOffset    = f;
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
