// ─── CudaPreprocess.cpp ────────────────────────────────────────
// GPU BGRA8 → FP32 CHW RGB preprocessing via NVRTC runtime compilation.
//
// Completely avoids build-time nvcc dependency. The CUDA kernel is
// stored as a string and compiled by NVRTC at init time. Compiled
// PTX is cached for the lifetime of the process.

#include "CudaPreprocess.h"

#include <cuda_runtime.h>
#include <cuda.h>
#include <nvrtc.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  CUDA kernel source (embedded string)
// ═══════════════════════════════════════════════════════════════

static const char* kKernelSource = R"(
extern "C" __global__ void Bgra8ToFp32ChwRgbKernel(
    const unsigned char* __restrict__ d_bgra,
    float* __restrict__ d_rgb_chw,
    int width,
    int height)
{
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    int srcIdx    = (y * width + x) * 4;
    int planeSize = width * height;
    int dstIdx    = y * width + x;

    unsigned char b = d_bgra[srcIdx + 0];
    unsigned char g = d_bgra[srcIdx + 1];
    unsigned char r = d_bgra[srcIdx + 2];

    d_rgb_chw[0 * planeSize + dstIdx] = __fdividef((float)r, 255.0f);
    d_rgb_chw[1 * planeSize + dstIdx] = __fdividef((float)g, 255.0f);
    d_rgb_chw[2 * planeSize + dstIdx] = __fdividef((float)b, 255.0f);
}
)";

// ═══════════════════════════════════════════════════════════════
//  Cached state (initialized once)
// ═══════════════════════════════════════════════════════════════

static bool        s_initialized = false;
static CUmodule    s_module      = nullptr;
static CUfunction  s_kernel      = nullptr;
static std::string s_buildLog;

static void CheckCudaError(CUresult err, const char* tag) {
    if (err != CUDA_SUCCESS) {
        const char* name = nullptr;
        cuGetErrorName(err, &name);
        const char* str = nullptr;
        cuGetErrorString(err, &str);
        fprintf(stderr, "[CudaPreprocess] %s FAILED: %s (%s)\n",
                tag, name ? name : "?", str ? str : "?");
    }
}

// ═══════════════════════════════════════════════════════════════
//  InitCudaPreprocess
// ═══════════════════════════════════════════════════════════════

bool InitCudaPreprocess() {
    if (s_initialized) return true;

    // ── 1. Create NVRTC program ───────────────────────────
    nvrtcProgram prog = nullptr;
    nvrtcResult nvr = nvrtcCreateProgram(&prog, kKernelSource,
        "Bgra8ToFp32ChwRgbKernel", 0, nullptr, nullptr);
    if (nvr != NVRTC_SUCCESS) {
        fprintf(stderr, "[CudaPreprocess] nvrtcCreateProgram FAILED: %s\n",
                nvrtcGetErrorString(nvr));
        return false;
    }

    // ── 2. Compile ────────────────────────────────────────
    // Use compute_86 for broad GPU support (PTX JIT handles newer archs)
    const char* opts[] = {
        "--gpu-architecture=compute_86",
        "-std=c++17",
        "-use_fast_math",
        "-restrict"
    };
    nvr = nvrtcCompileProgram(prog, 4, opts);

    // ── 3. Get compilation log (always, for diagnostics) ──
    size_t logSize = 0;
    nvrtcGetProgramLogSize(prog, &logSize);
    if (logSize > 1) {
        s_buildLog.resize(logSize);
        nvrtcGetProgramLog(prog, &s_buildLog[0]);
        fprintf(stderr, "[CudaPreprocess] NVRTC build log:\n%s\n",
                s_buildLog.c_str());
    }

    if (nvr != NVRTC_SUCCESS) {
        fprintf(stderr, "[CudaPreprocess] nvrtcCompileProgram FAILED: %s\n",
                nvrtcGetErrorString(nvr));
        nvrtcDestroyProgram(&prog);
        return false;
    }

    // ── 4. Get compiled PTX ───────────────────────────────
    size_t ptxSize = 0;
    nvrtcGetPTXSize(prog, &ptxSize);
    std::vector<char> ptx(ptxSize);
    nvrtcGetPTX(prog, ptx.data());
    nvrtcDestroyProgram(&prog);

    fprintf(stderr, "[CudaPreprocess] Kernel compiled. PTX: %zu bytes\n",
            ptxSize);

    // ── 5. Load PTX into CUDA module ──────────────────────
    CUresult cu = cuModuleLoadData(&s_module, ptx.data());
    if (cu != CUDA_SUCCESS) {
        CheckCudaError(cu, "cuModuleLoadData");
        return false;
    }

    // ── 6. Get kernel function handle ─────────────────────
    cu = cuModuleGetFunction(&s_kernel, s_module,
                             "Bgra8ToFp32ChwRgbKernel");
    if (cu != CUDA_SUCCESS) {
        CheckCudaError(cu, "cuModuleGetFunction");
        cuModuleUnload(s_module);
        s_module = nullptr;
        return false;
    }

    s_initialized = true;
    fprintf(stderr, "[CudaPreprocess] Ready. Kernel compiled at runtime via NVRTC.\n");
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  LaunchBgra8ToFp32ChwRgb
// ═══════════════════════════════════════════════════════════════

void LaunchBgra8ToFp32ChwRgb(
    const uint8_t* d_bgra,
    float*         d_rgb_chw,
    int            width,
    int            height,
    cudaStream_t   stream)
{
    if (!s_initialized) {
        fprintf(stderr, "[CudaPreprocess] NOT INITIALIZED — call InitCudaPreprocess() first\n");
        return;
    }

    // Kernel launch config
    const dim3 block(16, 16);
    const dim3 grid(
        (static_cast<unsigned int>(width)  + block.x - 1) / block.x,
        (static_cast<unsigned int>(height) + block.y - 1) / block.y);

    // Args: d_bgra, d_rgb_chw, width, height
    void* args[] = {
        const_cast<uint8_t**>(&d_bgra),
        &d_rgb_chw,
        const_cast<int*>(&width),
        const_cast<int*>(&height)
    };

    CUresult cu = cuLaunchKernel(
        s_kernel,
        grid.x,  grid.y,  1,       // grid
        block.x, block.y, 1,       // block
        0,                          // shared memory
        stream,                     // CUDA stream
        args,                       // kernel params
        nullptr                     // extra
    );
#ifdef _DEBUG
    CheckCudaError(cu, "cuLaunchKernel");
#else
    (void)cu;
#endif
}

} // namespace SynapseX
