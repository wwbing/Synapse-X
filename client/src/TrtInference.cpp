// ─── TrtInference.cpp ───────────────────────────────────────────
// TensorRT 10.16 inference with GPU-side BGRA→FP32 CHW preprocessing.
//
// Pipeline (all on dedicated CUDA stream, single cudaStreamSynchronize):
//   1. cudaMemcpyAsync  H→D   BGRA uint8  (raw, 1.66 MB for 416²)
//   2. LaunchBgra8ToFp32ChwRgb  GPU kernel  (BGRA→FP32 CHW RGB)
//   3. enqueueV3(stream)         GPU inference
//   4. cudaStreamSynchronize     wait for entire GPU pipeline
//   5. cudaMemcpy          D→H   output detections (7.2 KB)
//
// PCIe savings: we transfer raw BGRA (~1.66 MB) instead of FP32 CHW
// (~2.0 MB). The larger win is eliminating the CPU for-loop over 173K
// pixels — GPU kernel does it in ~15–30 μs.

#include "TrtInference.h"
#include "CudaPreprocess.h"

#include <cuda_runtime.h>
#include <NvInfer.h>
#include <NvInferRuntime.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <vector>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  TRT Logger (singleton)
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

    // ── 1. Load engine ────────────────────────────────────
    auto engineData = LoadFile(enginePath);
    if (engineData.empty()) return false;

    // ── 2. Deserialize ────────────────────────────────────
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

    // ── 4. Allocate GPU buffers (IO tensors + BGRA staging) ─
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

    // ── 5. Allocate BGRA staging buffer on GPU ─────────────
    size_t bgraBytes = static_cast<size_t>(m_modelW) * m_modelH * 4;
    cudaError_t cudaErr = cudaMalloc(&m_dBgraInput, bgraBytes);
    if (cudaErr != cudaSuccess) {
        fprintf(stderr, "[TrtInference] cudaMalloc(BGRA %zux%zux4) FAILED: %s\n",
                static_cast<size_t>(m_modelW), static_cast<size_t>(m_modelH),
                cudaGetErrorString(cudaErr));
        Cleanup();
        return false;
    }

    m_initialized = true;
    fprintf(stderr, "[TrtInference] Ready. Model: %dx%d, %d dets, "
            "output: %zu B, BGRA buf: %zu B (GPU preprocess via NVRTC)\n",
            m_modelW, m_modelH, m_numDets, m_outputBytes, bgraBytes);
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
//  Infer (GPU pipeline — hot path)
// ═══════════════════════════════════════════════════════════════

std::vector<Detection> TrtInference::Infer(
    const uint8_t* bgra, float confThr) {

    std::vector<Detection> detections;
    if (!m_initialized) return detections;

    auto* stream = reinterpret_cast<cudaStream_t>(m_stream);
    const size_t bgraBytes = static_cast<size_t>(m_modelW) * m_modelH * 4;

    // ═══════════════════════════════════════════════════════════
    //  GPU pipeline — ALL async, queued on dedicated stream
    // ═══════════════════════════════════════════════════════════

    // ── 1. Transfer raw BGRA uint8 H→D ────────────────────
    cudaMemcpyAsync(m_dBgraInput, bgra, bgraBytes,
                    cudaMemcpyHostToDevice, stream);

    // ── 2. GPU kernel: BGRA uint8 → FP32 CHW RGB (NVRTC) ──
    LaunchBgra8ToFp32ChwRgb(
        static_cast<const uint8_t*>(m_dBgraInput),
        static_cast<float*>(m_dInput),
        m_modelW, m_modelH, stream);

    // ── 3. TRT inference ──────────────────────────────────
    auto* ctx = static_cast<nvinfer1::IExecutionContext*>(m_context);
    bool ok = ctx->enqueueV3(stream);
    if (!ok) {
        fprintf(stderr, "[TrtInference] enqueueV3 FAILED\n");
        cudaStreamSynchronize(stream);
        return detections;
    }

    // ── 4. Sync: wait for entire GPU pipeline ─────────────
    // This is the ONLY synchronization point. All GPU work
    // (copy, kernel, inference) completes before we read back.
    cudaStreamSynchronize(stream);

    // ── 5. Copy output D→H ────────────────────────────────
    std::vector<float> output(m_outputBytes / sizeof(float));
    cudaMemcpy(output.data(), m_dOutput, m_outputBytes,
               cudaMemcpyDeviceToHost);

    // ── 6. Postprocess ────────────────────────────────────
    for (int i = 0; i < m_numDets; ++i) {
        const float* row = &output[static_cast<size_t>(i) * 6];
        float conf = row[4];
        if (conf < confThr) continue;

        Detection det;
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
    if (m_dBgraInput) { cudaFree(m_dBgraInput); m_dBgraInput = nullptr; }
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
