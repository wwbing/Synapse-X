#pragma once

// ─── CudaPreprocess ───────────────────────────────────────────
// GPU-side BGRA8 → FP32 CHW RGB preprocessing via NVRTC.
//
// Uses NVRTC (NVIDIA Runtime Compilation) to compile the CUDA
// kernel at init time — no nvcc required at build time.
//
// InitCudaPreprocess():
//   Compiles the kernel, loads PTX, caches CUfunction handle.
//   Must be called once, after cudaSetDevice(), from any thread.
//
// LaunchBgra8ToFp32ChwRgb():
//   Queues the kernel on the given CUDA stream.
//   Thread-safe as long as the stream is not shared across threads.

#include <cstdint>
#include <cuda_runtime.h>

namespace SynapseX {

// One-time init. Returns true on success.
bool InitCudaPreprocess();

// Launch the BGRA→FP32 CHW RGB kernel on the given CUDA stream.
//   d_bgra:    GPU pointer to uint8 BGRA, size = width × height × 4
//   d_rgb_chw: GPU pointer to float RGB CHW, size = 3 × width × height
//   width, height: image dimensions (e.g. 416, 416)
//   stream:    CUDA stream to queue the kernel on
void LaunchBgra8ToFp32ChwRgb(
    const uint8_t* d_bgra,
    float*         d_rgb_chw,
    int            width,
    int            height,
    cudaStream_t   stream);

} // namespace SynapseX
