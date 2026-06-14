#pragma once

// ── LZ4 high-speed compression module ────────────────────────
// Wraps LZ4_compress_fast with pre-allocated buffers to avoid
// repeated allocations in the hot path.
//
// Usage:
//   Lz4Compressor comp;
//   comp.Initialize(640 * 640 * 4);   // pre-alloc for BGRA ROI
//   comp.Compress(input, size, output);

#include <cstdint>
#include <vector>

namespace SynapseX {

class Lz4Compressor {
public:
    Lz4Compressor() = default;

    // Pre-allocate internal compression buffer for the given max input size.
    // Call once during setup, not per frame.
    bool Initialize(int maxInputSize);

    // Compress `input` (inputSize bytes) into `output`.
    // output will be resized to exactly fit the compressed payload.
    // Returns true on success, false if compression failed.
    bool Compress(const uint8_t* input, int inputSize,
                  std::vector<uint8_t>& output);

    // Worst-case upper bound for compressed data size (LZ4_compressBound).
    static int GetMaxOutputSize(int inputSize);

    int GetMaxInputSize() const { return m_maxInputSize; }

private:
    int m_maxInputSize = 0;
    std::vector<uint8_t> m_compressBuf; // pre-allocated worst-case buffer
};

} // namespace SynapseX
