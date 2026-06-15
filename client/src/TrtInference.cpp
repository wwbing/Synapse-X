// ─── TrtInference.cpp ───────────────────────────────────────────
// TensorRT 10.16 inference module for Synapse-X Client.
//
// Hot path: Infer() does:
//   1. BGRA uint8 → FP32 CHW [0,1] RGB (CPU preprocess)
//   2. cudaMemcpy H→D
//   3. enqueueV3() — GPU inference
//   4. cudaMemcpy D→H
//   5. Postprocess: threshold + coordinate scaling
//
// All GPU memory is allocated once in Initialize(). Zero per-frame
// CUDA allocations.

#include "TrtInference.h"

#include <cuda_runtime.h>
#include <NvInfer.h>
#include <NvInferRuntime.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  TRT Logger (singleton, minimal)
// ═══════════════════════════════════════════════════════════════

class TrtLogger : public nvinfer1::ILogger {
public:
    static TrtLogger& Instance() {
        static TrtLogger logger;
        return logger;
    }
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) {
            fprintf(stderr, "[TRT] %s\n", msg);
        }
    }
private:
    TrtLogger() = default;
};

// ═══════════════════════════════════════════════════════════════
//  Helper: load entire file into memory
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

// ═══════════════════════════════════════════════════════════════
//  Helper: get element count from Dims
// ═══════════════════════════════════════════════════════════════

static size_t DimsVolume(const nvinfer1::Dims& dims) {
    size_t vol = 1;
    for (int i = 0; i < dims.nbDims; ++i) {
        vol *= (dims.d[i] > 0 ? static_cast<size_t>(dims.d[i]) : 1);
    }
    return vol;
}

// ═══════════════════════════════════════════════════════════════
//  Lifecycle
// ═══════════════════════════════════════════════════════════════

TrtInference::~TrtInference() {
    Cleanup();
}

bool TrtInference::Initialize(const std::string& enginePath,
                               int modelWidth,
                               int modelHeight,
                               int numDetections) {
    if (m_initialized) Cleanup();

    m_modelW  = modelWidth;
    m_modelH  = modelHeight;
    m_numDets = numDetections;

    // ── 1. Load engine file ───────────────────────────────
    auto engineData = LoadFile(enginePath);
    if (engineData.empty()) return false;

    // ── 2. Deserialize engine ─────────────────────────────
    auto* runtime = nvinfer1::createInferRuntime(TrtLogger::Instance());
    if (!runtime) {
        fprintf(stderr, "[TrtInference] createInferRuntime FAILED\n");
        return false;
    }
    m_runtime = runtime;

    auto* engine = runtime->deserializeCudaEngine(
        engineData.data(), engineData.size());
    if (!engine) {
        fprintf(stderr, "[TrtInference] deserializeCudaEngine FAILED\n");
        Cleanup();
        return false;
    }
    m_engine = engine;

    // ── 3. Create execution context ───────────────────────
    auto* ctx = engine->createExecutionContext();
    if (!ctx) {
        fprintf(stderr, "[TrtInference] createExecutionContext FAILED\n");
        Cleanup();
        return false;
    }
    m_context = ctx;

    // ── 4. Allocate GPU buffers ───────────────────────────
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
            Cleanup();
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

    m_initialized = true;
    fprintf(stderr, "[TrtInference] Ready. Model: %dx%d, %d detections, "
            "output: %zu bytes\n",
            m_modelW, m_modelH, m_numDets, m_outputBytes);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SetupStream — create dedicated CUDA stream
// ═══════════════════════════════════════════════════════════════

bool TrtInference::SetupStream() {
    if (!m_initialized) return false;
    if (m_stream) return true;  // already created

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
//  Infer (hot path)
// ═══════════════════════════════════════════════════════════════

std::vector<Detection> TrtInference::Infer(
    const uint8_t* bgra, float confThr) {

    std::vector<Detection> detections;
    if (!m_initialized) return detections;

    // ── 1. Preprocess: BGRA uint8 → FP32 CHW RGB [0,1] ───
    std::vector<float> input(static_cast<size_t>(m_modelW) * m_modelH * 3);

    const int planeSize = m_modelW * m_modelH;
    for (int y = 0; y < m_modelH; ++y) {
        for (int x = 0; x < m_modelW; ++x) {
            int srcIdx = (y * m_modelW + x) * 4;   // BGRA offset
            int dstIdx = y * m_modelW + x;          // H×W offset

            // Channel reorder: BGRA → RGB, normalize to [0, 1]
            input[0 * planeSize + dstIdx] = bgra[srcIdx + 2] / 255.0f; // R
            input[1 * planeSize + dstIdx] = bgra[srcIdx + 1] / 255.0f; // G
            input[2 * planeSize + dstIdx] = bgra[srcIdx + 0] / 255.0f; // B
            // Alpha (srcIdx+3) is ignored
        }
    }

    // ── 2. Copy input to GPU ──────────────────────────────
    cudaMemcpy(m_dInput, input.data(),
               input.size() * sizeof(float),
               cudaMemcpyHostToDevice);

    // ── 3. Enqueue inference (dedicated CUDA stream) ───────
    auto* ctx = static_cast<nvinfer1::IExecutionContext*>(m_context);
    auto* stream = reinterpret_cast<cudaStream_t>(m_stream);
    bool ok = ctx->enqueueV3(stream);
    if (!ok) {
        fprintf(stderr, "[TrtInference] enqueueV3 FAILED\n");
        return detections;
    }
    cudaStreamSynchronize(stream);

    // ── 4. Copy output back to CPU ────────────────────────
    std::vector<float> output(m_outputBytes / sizeof(float));
    cudaMemcpy(output.data(), m_dOutput, m_outputBytes,
               cudaMemcpyDeviceToHost);

    // ── 5. Postprocess: threshold, clamp to model dimensions ─
    for (int i = 0; i < m_numDets; ++i) {
        const float* row = &output[static_cast<size_t>(i) * 6];
        float conf = row[4];
        if (conf < confThr) continue;

        Detection det;
        // Coordinates stay in model pixel space — no scaling.
        // Host knows the ROI size and can map back to screen coords.
        det.x1         = std::max(0.0f, std::min(row[0], static_cast<float>(m_modelW) - 1));
        det.y1         = std::max(0.0f, std::min(row[1], static_cast<float>(m_modelH) - 1));
        det.x2         = std::max(0.0f, std::min(row[2], static_cast<float>(m_modelW) - 1));
        det.y2         = std::max(0.0f, std::min(row[3], static_cast<float>(m_modelH) - 1));
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
    if (m_stream) {
        cudaStreamDestroy(reinterpret_cast<cudaStream_t>(m_stream));
        m_stream = nullptr;
    }
    if (m_dInput)  { cudaFree(m_dInput);  m_dInput  = nullptr; }
    if (m_dOutput) { cudaFree(m_dOutput); m_dOutput = nullptr; }

    if (m_context) {
        delete static_cast<nvinfer1::IExecutionContext*>(m_context);
        m_context = nullptr;
    }
    if (m_engine) {
        delete static_cast<nvinfer1::ICudaEngine*>(m_engine);
        m_engine = nullptr;
    }
    if (m_runtime) {
        delete static_cast<nvinfer1::IRuntime*>(m_runtime);
        m_runtime = nullptr;
    }
    m_initialized = false;
}

} // namespace SynapseX
