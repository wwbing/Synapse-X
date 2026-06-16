#pragma once

// ─── TrtInference ──────────────────────────────────────────────
// TensorRT inference wrapper for Synapse-X Client pipeline.
//
// Usage:
//   TrtInference trt;
//   trt.Initialize("model/bf416.engine");
//   std::vector<Detection> dets = trt.Infer(bgraPixels, 0.25f);
//
// Threading: NOT thread-safe. Use from the same thread as UdpReceiver.
//
// Input:  BGRA uint8 pixels, modelWidth × modelHeight (e.g. 416×416)
// Output: vector of Detection { x1, y1, x2, y2, confidence, classId }
//         Coordinates are in model pixel space [0, modelW) × [0, modelH).

#include <cstdint>
#include <string>
#include <vector>

namespace SynapseX {

// ── Detection result ────────────────────────────────────────
struct Detection {
    float x1, y1;         // top-left corner (model pixel coords, no scaling)
    float x2, y2;         // bottom-right corner
    float confidence;
    int   classId;
};

// ── TensorRT inference engine ───────────────────────────────
class TrtInference {
public:
    TrtInference() = default;
    ~TrtInference();

    // Non-copyable, non-movable (owns GPU memory)
    TrtInference(const TrtInference&) = delete;
    TrtInference& operator=(const TrtInference&) = delete;
    TrtInference(TrtInference&&) = delete;
    TrtInference& operator=(TrtInference&&) = delete;

    // Load engine from file, allocate GPU buffers.
    // modelW/modelH: input dimensions the engine was built for.
    // numDetections: output detections count (usually 300).
    bool Initialize(const std::string& enginePath,
                    int modelWidth = 416,
                    int modelHeight = 416,
                    int numDetections = 300);

    // Create dedicated CUDA stream. Must be called from the thread that
    // will call Infer(), after cudaSetDevice().
    bool SetupStream();

    // Run inference on BGRA uint8 input.
    // All processing (BGRA→H→D → GPU preprocess → enqueueV3 → D→H)
    // happens on the dedicated CUDA stream. Only one cudaStreamSynchronize
    // is issued at the end of the pipeline.
    // Caller provides raw CPU BGRA data — this function handles the rest.
    std::vector<Detection> Infer(const uint8_t* bgra,
                                 float confThr = 0.25f);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }

    int GetModelWidth()  const { return m_modelW; }
    int GetModelHeight() const { return m_modelH; }

private:
    bool m_initialized = false;

    int m_modelW = 416;
    int m_modelH = 416;
    int m_numDets = 300;

    // TensorRT objects (opaque to caller)
    void* m_runtime   = nullptr;  // nvinfer1::IRuntime*
    void* m_engine    = nullptr;  // nvinfer1::ICudaEngine*
    void* m_context   = nullptr;  // nvinfer1::IExecutionContext*

    // GPU buffers + stream
    void*  m_dInput     = nullptr;  // TRT input  (FP32 CHW, 3×W×H floats)
    void*  m_dBgraInput = nullptr;  // raw BGRA   (uint8,  W×H×4 bytes)
    void*  m_dOutput    = nullptr;  // TRT output (FP32)
    size_t m_outputBytes = 0;
    void*  m_stream     = nullptr;  // cudaStream_t
};

} // namespace SynapseX
