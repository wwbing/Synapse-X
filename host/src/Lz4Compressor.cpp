// ─── Lz4Compressor.cpp ───────────────────────────────────────
// Thin wrapper around LZ4_compress_fast.
// All heavy lifting (buffer allocation) happens in Initialize().
// Compress() is a hot-path call with zero allocations.

#include "Lz4Compressor.h"
#include <lz4.h>

namespace SynapseX {

bool Lz4Compressor::Initialize(int maxInputSize) {
    if (maxInputSize <= 0) {
        return false;
    }
    m_maxInputSize = maxInputSize;
    int bound = LZ4_compressBound(maxInputSize);
    if (bound <= 0) {
        return false;
    }
    m_compressBuf.resize(static_cast<size_t>(bound));
    return true;
}

bool Lz4Compressor::Compress(const uint8_t* input, int inputSize,
                             std::vector<uint8_t>& output) {
    // Auto-resize if the current input exceeds our pre-allocated buffer.
    // This should not happen during steady-state operation — it's a safety net.
    if (inputSize > m_maxInputSize) {
        if (!Initialize(inputSize)) {
            return false;
        }
    }

    int srcSize = inputSize;
    int dstCap  = static_cast<int>(m_compressBuf.size());

    // LZ4_compress_fast: acceleration=5 trades ~5% compression ratio
    // for ~50% less CPU time. Game scenes with grass/particles/noise
    // cause excessive hash probing at accel=1, blowing out CPU budget.
    // At accel=5, compression stays well under 0.5ms even on noisy frames.
    int compressedSize = LZ4_compress_fast(
        reinterpret_cast<const char*>(input),
        reinterpret_cast<char*>(m_compressBuf.data()),
        srcSize,
        dstCap,
        5  // acceleration — speed > ratio for real-time pipeline
    );

    if (compressedSize <= 0) {
        // Compression failed (output buffer too small or internal error).
        return false;
    }

    // Copy only the valid compressed bytes into the caller's output.
    output.assign(m_compressBuf.data(),
                  m_compressBuf.data() + compressedSize);
    return true;
}

int Lz4Compressor::GetMaxOutputSize(int inputSize) {
    return LZ4_compressBound(inputSize);
}

} // namespace SynapseX
