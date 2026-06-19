#pragma once

// ─── TrtInference ──────────────────────────────────────────────
// TensorRT inference wrapper with hot-swappable engine support.
//
// Usage:
//   TrtInference trt;
//   trt.Initialize();                        // create CUDA runtime
//   trt.LoadEngine(0);                       // load model ID 0
//   // In hot loop:
//   auto dets = trt.Infer(bgraPixels, 0.25f); // auto-switches model
//                                             // if g_targetModelId changed
//
// Threading: Infer() called from Consumer thread ONLY.
// g_targetModelId written by Producer thread (UdpReceiver).

#include <cstdint>
#include <string>
#include <vector>

namespace SynapseX {

struct Detection {
    float x1, y1;
    float x2, y2;
    float confidence;
    int   classId;
};

class TrtInference {
public:
    TrtInference() = default;
    ~TrtInference();

    TrtInference(const TrtInference&) = delete;
    TrtInference& operator=(const TrtInference&) = delete;
    TrtInference(TrtInference&&) = delete;
    TrtInference& operator=(TrtInference&&) = delete;

    // Create CUDA runtime. No engine loaded yet.
    // Call once, then call LoadEngine() or let Infer() auto-switch.
    bool Initialize();

    // Load a specific model by ID. Destroys old engine if any.
    //  0: apex_enemy_416     1: delta_body_head_416
    //  2: bf6_enemy_self_416  3: ow2_enemy_416
    bool LoadEngine(uint8_t modelId);

    // Load engine directly by file path (for standalone testing).
    // Destroys old engine if any. modelId is just for tracking.
    bool LoadEngineByPath(const std::string& path, uint8_t modelId = 254);

    // Create dedicated CUDA stream. Call once from Consumer thread.
    bool SetupStream();

    // Run inference. If g_targetModelId changed since last call,
    // synchronizes stream, destroys old engine, loads new model,
    // and returns empty (caller should feed next frame).
    std::vector<Detection> Infer(const uint8_t* bgra,
                                 float confThr = 0.25f);

    void Cleanup();
    bool IsInitialized() const { return m_initialized; }
    bool HasEngine()      const { return m_context != nullptr; }

    int GetModelWidth()   const { return m_modelW; }
    int GetModelHeight()  const { return m_modelH; }
    uint8_t GetCurrentModelId() const { return m_currentModelId; }

private:
    // Map modelId → engine file path
    static std::string GetModelPath(uint8_t modelId);

    // Destroy TRT engine + context + GPU buffers (keep runtime + stream)
    void UnloadEngine();

    // Load engine from file, deserialize, create context, allocate IO
    bool LoadEngineFile(const std::string& path);

    bool m_initialized = false;

    int m_modelW = 416;
    int m_modelH = 416;
    int m_numDets = 300;

    uint8_t m_currentModelId = 255;  // invalid → forces first load

    // TRT objects
    void* m_runtime   = nullptr;  // nvinfer1::IRuntime* (persists across reloads)
    void* m_engine    = nullptr;  // nvinfer1::ICudaEngine*
    void* m_context   = nullptr;  // nvinfer1::IExecutionContext*

    // GPU buffers
    void*  m_dInput     = nullptr;
    void*  m_dBgraInput = nullptr;
    void*  m_dOutput    = nullptr;
    size_t m_outputBytes = 0;
    void*  m_stream     = nullptr;  // cudaStream_t (persists across reloads)
};

} // namespace SynapseX
