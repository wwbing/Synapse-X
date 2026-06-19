// ─── TrtInference.cpp ───────────────────────────────────────────
// TensorRT 10.16 inference with hot-swappable engine support.
//
// Engine hot-swap protocol:
//   Producer (UdpReceiver) writes g_targetModelId from PacketHeader.
//   Consumer (Infer) checks at start of each frame.
//   If changed: sync stream → destroy old engine → load new → return empty.
//   Next call: normal inference with new model.
//
// Model ID mapping (→ model/engine/<name>.engine):
//   0: apex_enemy_416       1: delta_body_head_416
//   2: bf6_enemy_self_416    3: ow2_enemy_416

#include "TrtInference.h"
#include "CudaPreprocess.h"
#include "PacketHeader.h"  // g_targetModelId extern

#include <cuda_runtime.h>
#include <NvInfer.h>
#include <NvInferRuntime.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <vector>

// ── Global definition (declared extern in PacketHeader.h) ─
std::atomic<uint8_t> g_targetModelId{0};

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  TRT Logger
// ═══════════════════════════════════════════════════════════════

class TrtLogger : public nvinfer1::ILogger {
public:
    static TrtLogger& Instance() {
        static TrtLogger logger;
        return logger;
    }
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING)
            fprintf(stderr, "[TRT] %s\n", msg);
    }
private:
    TrtLogger() = default;
};

// ═══════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════

static std::vector<char> LoadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        fprintf(stderr, "[TrtInference] Cannot open: %s\n", path.c_str());
        return {};
    }
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<char> data(size);
    f.read(data.data(), size);
    fprintf(stderr, "[TrtInference] Loaded engine: %s (%.1f MB)\n",
            path.c_str(), size / 1048576.0);
    return data;
}

static size_t DimsVolume(const nvinfer1::Dims& dims) {
    size_t vol = 1;
    for (int i = 0; i < dims.nbDims; ++i)
        vol *= (dims.d[i] > 0 ? static_cast<size_t>(dims.d[i]) : 1);
    return vol;
}

// ═══════════════════════════════════════════════════════════════
//  Model ID → path mapping
// ═══════════════════════════════════════════════════════════════

std::string TrtInference::GetModelPath(uint8_t modelId) {
    switch (modelId) {
        case 0:  return "../../model/engine/apex_enemy_416.engine";       // Apex Legends, 1 class: enemy
        case 1:  return "../../model/engine/delta_body_head_416.engine";  // Delta Force, 2 classes: body, head
        case 2:  return "../../model/engine/bf6_enemy_self_416.engine";   // Battlefield 6, 2 classes: enemy, teammate
        case 3:  return "../../model/engine/ow2_enemy_416.engine";        // Overwatch 2, 1 class: enemy
        default:
            fprintf(stderr, "[TrtInference] Unknown modelId=%u (valid: 0-3)\n", modelId);
            return "";
    }
}

// ═══════════════════════════════════════════════════════════════
//  Initialize — create runtime only (no engine)
// ═══════════════════════════════════════════════════════════════

TrtInference::~TrtInference() {
    Cleanup();
}

bool TrtInference::Initialize() {
    if (m_initialized) Cleanup();

    auto* runtime = nvinfer1::createInferRuntime(TrtLogger::Instance());
    if (!runtime) {
        fprintf(stderr, "[TrtInference] createInferRuntime FAILED\n");
        return false;
    }
    m_runtime = runtime;
    m_initialized = true;
    fprintf(stderr, "[TrtInference] Runtime ready. Waiting for engine load...\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  UnloadEngine — destroy TRT engine + context + GPU buffers
// ═══════════════════════════════════════════════════════════════

void TrtInference::UnloadEngine() {
    if (m_dBgraInput) { cudaFree(m_dBgraInput); m_dBgraInput = nullptr; }
    if (m_dInput)     { cudaFree(m_dInput);     m_dInput     = nullptr; }
    if (m_dOutput)    { cudaFree(m_dOutput);    m_dOutput    = nullptr; }
    m_outputBytes = 0;

    // Context MUST be destroyed before engine
    if (m_context) {
        delete static_cast<nvinfer1::IExecutionContext*>(m_context);
        m_context = nullptr;
    }
    if (m_engine) {
        delete static_cast<nvinfer1::ICudaEngine*>(m_engine);
        m_engine = nullptr;
    }
}

// ═══════════════════════════════════════════════════════════════
//  LoadEngineFile — deserialize engine, create context, alloc IO
// ═══════════════════════════════════════════════════════════════

bool TrtInference::LoadEngineFile(const std::string& path) {
    if (!m_runtime) return false;
    if (path.empty()) return false;

    // ── 1. Load file ────────────────────────────────────────
    auto engineData = LoadFile(path);
    if (engineData.empty()) return false;

    // ── 2. Deserialize engine ────────────────────────────────
    auto* engine = static_cast<nvinfer1::IRuntime*>(m_runtime)
        ->deserializeCudaEngine(engineData.data(), engineData.size());
    if (!engine) {
        fprintf(stderr, "[TrtInference] deserializeCudaEngine FAILED for %s\n",
                path.c_str());
        return false;
    }
    m_engine = engine;

    // ── 3. Create execution context ──────────────────────────
    auto* ctx = engine->createExecutionContext();
    if (!ctx) {
        fprintf(stderr, "[TrtInference] createExecutionContext FAILED\n");
        return false;
    }
    m_context = ctx;

    // ── 4. Allocate GPU IO buffers ───────────────────────────
    int nbTensors = engine->getNbIOTensors();
    for (int i = 0; i < nbTensors; ++i) {
        const char* name = engine->getIOTensorName(i);
        nvinfer1::Dims dims = engine->getTensorShape(name);
        nvinfer1::DataType dt = engine->getTensorDataType(name);
        nvinfer1::TensorIOMode mode = engine->getTensorIOMode(name);

        size_t vol = DimsVolume(dims);
        size_t elemSize = (dt == nvinfer1::DataType::kFLOAT) ? 4 : 2;
        size_t bytes = vol * elemSize;

        void* devPtr = nullptr;
        cudaError_t err = cudaMalloc(&devPtr, bytes);
        if (err != cudaSuccess) {
            fprintf(stderr, "[TrtInference] cudaMalloc FAILED for '%s': %s\n",
                    name, cudaGetErrorString(err));
            UnloadEngine();
            return false;
        }

        if (mode == nvinfer1::TensorIOMode::kINPUT) {
            ctx->setInputTensorAddress(name, devPtr);
            m_dInput = devPtr;
        } else {
            ctx->setOutputTensorAddress(name, devPtr);
            m_dOutput    = devPtr;
            m_outputBytes = bytes;
        }
    }

    // ── 5. Allocate BGRA staging buffer ─────────────────────
    size_t bgraBytes = static_cast<size_t>(m_modelW) * m_modelH * 4;
    cudaError_t cudaErr = cudaMalloc(&m_dBgraInput, bgraBytes);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[TrtInference] cudaMalloc(BGRA) FAILED: %s\n",
                cudaGetErrorString(cudaErr));
        UnloadEngine();
        return false;
    }

    fprintf(stderr, "[TrtInference] Engine loaded: %s | %dx%d, output: %zu B\n",
            path.c_str(), m_modelW, m_modelH, m_outputBytes);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LoadEngine — public, maps modelId to path and loads
// ═══════════════════════════════════════════════════════════════

bool TrtInference::LoadEngine(uint8_t modelId) {
    if (!m_initialized) return false;

    std::string path = GetModelPath(modelId);
    if (path.empty()) return false;

    // Destroy old engine first (if any)
    UnloadEngine();

    if (!LoadEngineFile(path)) {
        fprintf(stderr, "[TrtInference] LoadEngine(%u) FAILED\n", modelId);
        return false;
    }

    m_currentModelId = modelId;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LoadEngineByPath — for standalone testing
// ═══════════════════════════════════════════════════════════════

bool TrtInference::LoadEngineByPath(const std::string& path, uint8_t modelId) {
    if (!m_initialized) return false;
    UnloadEngine();
    if (!LoadEngineFile(path)) return false;
    m_currentModelId = modelId;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SetupStream
// ═══════════════════════════════════════════════════════════════

bool TrtInference::SetupStream() {
    if (!m_initialized) return false;
    if (m_stream) return true;

    cudaError_t err = cudaStreamCreateWithFlags(
        reinterpret_cast<cudaStream_t*>(&m_stream),
        cudaStreamNonBlocking);
    if (err != cudaSuccess) {
        fprintf(stderr, "[TrtInference] cudaStreamCreate FAILED: %s\n",
                cudaGetErrorString(err));
        return false;
    }
    fprintf(stderr, "[TrtInference] CUDA stream created (non-blocking).\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Infer — with hot-swap check
// ═══════════════════════════════════════════════════════════════

std::vector<Detection> TrtInference::Infer(
    const uint8_t* bgra, float confThr) {

    std::vector<Detection> detections;
    if (!m_initialized) return detections;

    auto* stream = reinterpret_cast<cudaStream_t>(m_stream);

    // ═══════════════════════════════════════════════════════════
    //  Hot-swap check: did the host request a different model?
    // ═══════════════════════════════════════════════════════════
    uint8_t targetId = g_targetModelId.load(std::memory_order_relaxed);
    if (m_currentModelId != targetId) {
        fprintf(stderr, "[TrtInference] Model switch: %u → %u\n",
                m_currentModelId, targetId);

        // 1. Sync stream — ensure previous frame's GPU work is done
        if (stream) cudaStreamSynchronize(stream);

        // 2. Destroy old engine + context + IO buffers
        UnloadEngine();

        // 3. Load new engine
        std::string path = GetModelPath(targetId);
        if (path.empty() || !LoadEngineFile(path)) {
            fprintf(stderr, "[TrtInference] Hot-swap FAILED for modelId=%u. "
                    "Inference disabled until valid modelId received.\n",
                    targetId);
            m_currentModelId = 255;  // force retry
            return detections;
        }

        m_currentModelId = targetId;
        fprintf(stderr, "[TrtInference] Hot-swap OK. Now using model %u: %s\n",
                targetId, path.c_str());

        // 4. Return empty — discard stale frame (old image ≠ new model)
        return detections;
    }

    // No engine loaded yet (or hot-swap failed)
    if (!m_context) return detections;

    // ═══════════════════════════════════════════════════════════
    //  Normal GPU pipeline
    // ═══════════════════════════════════════════════════════════

    const size_t bgraBytes = static_cast<size_t>(m_modelW) * m_modelH * 4;

    // 1. Transfer raw BGRA H→D
    cudaMemcpyAsync(m_dBgraInput, bgra, bgraBytes,
                    cudaMemcpyHostToDevice, stream);

    // 2. GPU kernel: BGRA → FP32 CHW RGB
    LaunchBgra8ToFp32ChwRgb(
        static_cast<const uint8_t*>(m_dBgraInput),
        static_cast<float*>(m_dInput),
        m_modelW, m_modelH, stream);

    // 3. TRT inference
    auto* ctx = static_cast<nvinfer1::IExecutionContext*>(m_context);
    bool ok = ctx->enqueueV3(stream);
    if (!ok) {
        fprintf(stderr, "[TrtInference] enqueueV3 FAILED\n");
        cudaStreamSynchronize(stream);
        return detections;
    }

    // 4. Sync
    cudaStreamSynchronize(stream);

    // 5. Copy output D→H
    std::vector<float> output(m_outputBytes / sizeof(float));
    cudaMemcpy(output.data(), m_dOutput, m_outputBytes,
               cudaMemcpyDeviceToHost);

    // 6. Postprocess
    for (int i = 0; i < m_numDets; ++i) {
        const float* row = &output[static_cast<size_t>(i) * 6];
        float conf = row[4];
        if (conf < confThr) continue;

        Detection det;
        det.x1 = std::max(0.0f, std::min(row[0], static_cast<float>(m_modelW) - 1));
        det.y1 = std::max(0.0f, std::min(row[1], static_cast<float>(m_modelH) - 1));
        det.x2 = std::max(0.0f, std::min(row[2], static_cast<float>(m_modelW) - 1));
        det.y2 = std::max(0.0f, std::min(row[3], static_cast<float>(m_modelH) - 1));
        det.confidence = conf;
        det.classId    = static_cast<int>(row[5]);
        detections.push_back(det);
    }

    return detections;
}

// ═══════════════════════════════════════════════════════════════
//  Cleanup
// ═══════════════════════════════════════════════════════════════

void TrtInference::Cleanup() {
    UnloadEngine();  // GPU buffers + context + engine
    if (m_stream) {
        cudaStreamDestroy(reinterpret_cast<cudaStream_t>(m_stream));
        m_stream = nullptr;
    }
    if (m_runtime) {
        delete static_cast<nvinfer1::IRuntime*>(m_runtime);
        m_runtime = nullptr;
    }
    m_initialized = false;
    m_currentModelId = 255;
}

} // namespace SynapseX
