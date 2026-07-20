// ─── TrtInference.cpp ───────────────────────────────────────────
// TensorRT 10.16 推理，支持热切换引擎。
//
// 引擎热切换协议：
//   Producer (UdpReceiver) 从 PacketHeader 写入 g_targetModelId。
//   Consumer (Infer) 在每帧开始时检查。
//   如果已更改：同步流 → 销毁旧引擎 → 加载新引擎 → 返回空结果。
//   下次调用：使用新模型正常推理。
//
// 模型ID映射（→ model/engine/<名称>.engine）：
//   0: apex_enemy_416       1: delta_body_head_416
//   2: bf6_enemy_self_416    3: ow2_enemy_416

#include "TrtInference.h"
#include "CudaPreprocess.h"
#include "Log.h"
#include "PacketHeader.h"  // g_targetModelId extern

#include <cuda_runtime.h>
#include <NvInfer.h>
#include <NvInferRuntime.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <vector>

// ── 全局定义（在 PacketHeader.h 中声明为 extern）────
std::atomic<uint8_t> g_targetModelId{0};

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  TRT 日志器
// ═══════════════════════════════════════════════════════════════

class TrtLogger : public nvinfer1::ILogger {
public:
    static TrtLogger& Instance() {
        static TrtLogger logger;
        return logger;
    }
    void log(Severity severity, const char* msg) noexcept override {
        if (severity == Severity::kINTERNAL_ERROR || severity == Severity::kERROR) {
            SX_LOG_ERROR("[TRT] {}", msg);
        } else if (severity == Severity::kWARNING) {
            SX_LOG_WARN("[TRT] {}", msg);
        } else {
            SX_LOG_DEBUG("[TRT] {}", msg);
        }
    }
private:
    TrtLogger() = default;
};

// ═══════════════════════════════════════════════════════════════
//  辅助函数
// ═══════════════════════════════════════════════════════════════

static std::vector<char> LoadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        SX_LOG_ERROR("[TrtInference] 无法打开引擎文件: {}", path);
        return {};
    }
    size_t size = f.tellg();
    f.seekg(0);
    std::vector<char> data(size);
    f.read(data.data(), size);
    SX_LOG_DEBUG("[TrtInference] 已加载引擎文件: 路径={} 大小MB={:.1f}",
                 path, size / 1048576.0);
    return data;
}

static size_t DimsVolume(const nvinfer1::Dims& dims) {
    size_t vol = 1;
    for (int i = 0; i < dims.nbDims; ++i)
        vol *= (dims.d[i] > 0 ? static_cast<size_t>(dims.d[i]) : 1);
    return vol;
}

// ═══════════════════════════════════════════════════════════════
//  模型ID → 路径映射
// ═══════════════════════════════════════════════════════════════

std::string TrtInference::GetModelPath(uint8_t modelId) {
    switch (modelId) {
        case 0:  return "../../model/engine/apex_enemy_self_416.engine"; // Apex Legends(新)，2类：队友, 敌人
        case 1:  return "../../model/engine/delta_body_head_416.engine";  // Delta Force，2类：身体，头部
        case 2:  return "../../model/engine/bf6_enemy_self_new.engine";   // Battlefield 6，2类：敌人，队友
        case 3:  return "../../model/engine/ow2_enemy_416.engine";        // Overwatch 2，1类：敌人
        case 4:  return "../../model/engine/aimlabs_enemy_416.engine";    // Aimlabs，1类：敌人
        case 5:  return "../../model/engine/pubg_body_head_416.engine";   // PUBG，2类：身体，头部
        case 6:  return "../../model/engine/cf_body_head_416.engine";     // CrossFire，2类：身体，头部
        case 7:  return "../../model/engine/cs2_enemy_self_416.engine";   // CS2，2类：CT, T
        default:
            SX_LOG_ERROR("[TrtInference] 未知的 modelId={} (有效范围: 0..7)", modelId);
            return "";
    }
}

// ═══════════════════════════════════════════════════════════════
//  Initialize — 仅创建运行时（无引擎）
// ═══════════════════════════════════════════════════════════════

TrtInference::~TrtInference() {
    Cleanup();
}

bool TrtInference::Initialize() {
    if (m_initialized) Cleanup();

    auto* runtime = nvinfer1::createInferRuntime(TrtLogger::Instance());
    if (!runtime) {
        SX_LOG_ERROR("[TrtInference] createInferRuntime 失败");
        return false;
    }
    m_runtime = runtime;
    m_initialized = true;
    SX_LOG_INFO("[TrtInference] 运行时已就绪；等待引擎加载");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  UnloadEngine — 销毁TRT引擎 + 上下文 + GPU缓冲区
// ═══════════════════════════════════════════════════════════════

void TrtInference::UnloadEngine() {
    if (m_dBgraInput) { cudaFree(m_dBgraInput); m_dBgraInput = nullptr; }
    if (m_dInput)     { cudaFree(m_dInput);     m_dInput     = nullptr; }
    if (m_dOutput)    { cudaFree(m_dOutput);    m_dOutput    = nullptr; }
    m_outputBytes = 0;

    // 上下文必须在引擎之前销毁
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
//  LoadEngineFile — 反序列化引擎，创建上下文，分配IO
// ═══════════════════════════════════════════════════════════════

bool TrtInference::LoadEngineFile(const std::string& path) {
    if (!m_runtime) return false;
    if (path.empty()) return false;

    // ── 1. 加载文件 ─────────────────────────────────────────
    auto engineData = LoadFile(path);
    if (engineData.empty()) return false;

    // ── 2. 反序列化引擎 ─────────────────────────────────────
    auto* engine = static_cast<nvinfer1::IRuntime*>(m_runtime)
        ->deserializeCudaEngine(engineData.data(), engineData.size());
    if (!engine) {
        SX_LOG_ERROR("[TrtInference] deserializeCudaEngine 失败:  {}", path);
        return false;
    }
    m_engine = engine;

    // ── 3. 创建执行上下文 ───────────────────────────────────
    auto* ctx = engine->createExecutionContext();
    if (!ctx) {
        SX_LOG_ERROR("[TrtInference] createExecutionContext 失败");
        return false;
    }
    m_context = ctx;

    // ── 4. 分配GPU IO缓冲区 ─────────────────────────────────
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
            SX_LOG_ERROR("[TrtInference] cudaMalloc 失败 (张量 '{}': {}",
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

    // ── 5. 分配BGRA暂存缓冲区 ─────────────────────────────
    size_t bgraBytes = static_cast<size_t>(m_modelW) * m_modelH * 4;
    cudaError_t cudaErr = cudaMalloc(&m_dBgraInput, bgraBytes);
    if (cudaErr != cudaSuccess) {
        SX_LOG_ERROR("[TrtInference] BGRA 输入 cudaMalloc 失败: {}",
                     cudaGetErrorString(cudaErr));
        UnloadEngine();
        return false;
    }

    SX_LOG_INFO("[TrtInference] 引擎已加载: 路径={} 输入={}x{} 输出字节={}",
                path, m_modelW, m_modelH, m_outputBytes);
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LoadEngine — 公共方法，映射 modelId 到路径并加载
// ═══════════════════════════════════════════════════════════════

bool TrtInference::LoadEngine(uint8_t modelId) {
    if (!m_initialized) return false;

    std::string path = GetModelPath(modelId);
    if (path.empty()) return false;

    // 先销毁旧引擎（如果有）
    UnloadEngine();

    if (!LoadEngineFile(path)) {
        SX_LOG_ERROR("[TrtInference] LoadEngine({}) 失败", modelId);
        return false;
    }

    m_currentModelId = modelId;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LoadEngineByPath — 用于独立测试
// ═══════════════════════════════════════════════════════════════

bool TrtInference::LoadEngineByPath(const std::string& path, uint8_t modelId) {
    if (!m_initialized) return false;
    UnloadEngine();
    if (!LoadEngineFile(path)) return false;
    m_currentModelId = modelId;
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SetupStream（设置CUDA流）
// ═══════════════════════════════════════════════════════════════

bool TrtInference::SetupStream() {
    if (!m_initialized) return false;
    if (m_stream) return true;

    cudaError_t err = cudaStreamCreateWithFlags(
        reinterpret_cast<cudaStream_t*>(&m_stream),
        cudaStreamNonBlocking);
    if (err != cudaSuccess) {
        SX_LOG_ERROR("[TrtInference] cudaStreamCreateWithFlags 失败: {}",
                     cudaGetErrorString(err));
        return false;
    }
    SX_LOG_INFO("[TrtInference] CUDA 流已创建 (非阻塞)");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Infer — 带热切换检查
// ═══════════════════════════════════════════════════════════════

std::vector<Detection> TrtInference::Infer(
    const uint8_t* bgra, float confThr) {

    std::vector<Detection> detections;
    if (!m_initialized) return detections;

    auto* stream = reinterpret_cast<cudaStream_t>(m_stream);

    // ═══════════════════════════════════════════════════════════
    //  热切换检查：主机是否请求了不同的模型？
    // ═══════════════════════════════════════════════════════════
    uint8_t targetId = g_targetModelId.load(std::memory_order_relaxed);
    if (m_currentModelId != targetId) {
        SX_LOG_INFO("[TrtInference] 请求模型切换: {} -> {}",
                    m_currentModelId, targetId);

        // 1. 同步流 — 确保前一帧的GPU工作已完成
        if (stream) cudaStreamSynchronize(stream);

        // 2. 销毁旧引擎 + 上下文 + IO缓冲区
        UnloadEngine();

        // 3. 加载新引擎
        std::string path = GetModelPath(targetId);
        if (path.empty() || !LoadEngineFile(path)) {
            SX_LOG_ERROR("[TrtInference] 热切换失败 (modelId={})；推理已禁用，等待有效模型ID",
                         targetId);
            m_currentModelId = 255;  // 强制重试
            return detections;
        }

        m_currentModelId = targetId;
        SX_LOG_INFO("[TrtInference] 热切换成功: modelId={} 路径={}",
                    targetId, path);

        // 4. 返回空结果 — 丢弃过时帧（旧图像 ≠ 新模型）
        return detections;
    }

    // 尚未加载引擎（或热切换失败）
    if (!m_context) return detections;

    // ═══════════════════════════════════════════════════════════
    //  正常GPU流水线
    // ═══════════════════════════════════════════════════════════

    const size_t bgraBytes = static_cast<size_t>(m_modelW) * m_modelH * 4;

    // 1. 传输原始BGRA 主机→设备
    cudaMemcpyAsync(m_dBgraInput, bgra, bgraBytes,
                    cudaMemcpyHostToDevice, stream);

    // 2. GPU内核：BGRA → FP32 CHW RGB
    LaunchBgra8ToFp32ChwRgb(
        static_cast<const uint8_t*>(m_dBgraInput),
        static_cast<float*>(m_dInput),
        m_modelW, m_modelH, stream);

    // 3. TRT 推理
    auto* ctx = static_cast<nvinfer1::IExecutionContext*>(m_context);
    bool ok = ctx->enqueueV3(stream);
    if (!ok) {
        SX_LOG_ERROR("[TrtInference] enqueueV3 失败");
        cudaStreamSynchronize(stream);
        return detections;
    }

    // 4. 同步
    cudaStreamSynchronize(stream);

    // 5. 复制输出 设备→主机
    std::vector<float> output(m_outputBytes / sizeof(float));
    cudaMemcpy(output.data(), m_dOutput, m_outputBytes,
               cudaMemcpyDeviceToHost);

    // 6. 后处理
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
//  Cleanup（清理）
// ═══════════════════════════════════════════════════════════════

void TrtInference::Cleanup() {
    UnloadEngine();  // GPU缓冲区 + 上下文 + 引擎
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
