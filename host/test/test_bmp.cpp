// ─── BMP capture test ────────────────────────────────────────
// Validates DxgiCapturer ROI capture.
// Captures ONE frame -> writes 32-bit BMP -> exits.
//
// Build:
//   cd host
//   cmake --build build_x64 --config RelWithDebInfo --target SynapseX_Host_TestBmp
//   .\build_x64\RelWithDebInfo\SynapseX_Host_TestBmp.exe

#include "DxgiCapturer.h"
#include "Lz4Compressor.h"

#include <windows.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// ═══════════════════════════════════════════════════════════════
//  Hand-rolled BMP writer (zero 3rd-party deps)
//  Input: BGRA pixels (DXGI_FORMAT_B8G8R8A8_UNORM native format)
//  Output: 32-bit BMP, compatible with all image viewers
// ═══════════════════════════════════════════════════════════════

bool SaveBgraAsBmp(const char* path,
                   const uint8_t* pixels,
                   int width,
                   int height)
{
    int rowSize   = width * 4;                       // BGRA = 4 bytes/pixel
    int padSize   = (4 - (rowSize % 4)) % 4;
    int rowStride = rowSize + padSize;
    int imageSize = rowStride * height;

    // BITMAPFILEHEADER (14 bytes)
    BITMAPFILEHEADER bf = {};
    bf.bfType      = 0x4D42;                         // 'BM'
    bf.bfSize      = sizeof(BITMAPFILEHEADER)
                   + sizeof(BITMAPINFOHEADER)
                   + imageSize;
    bf.bfReserved1 = 0;
    bf.bfReserved2 = 0;
    bf.bfOffBits   = sizeof(BITMAPFILEHEADER)
                   + sizeof(BITMAPINFOHEADER);

    // BITMAPINFOHEADER (40 bytes)
    BITMAPINFOHEADER bi = {};
    bi.biSize          = sizeof(BITMAPINFOHEADER);
    bi.biWidth         = width;
    bi.biHeight        = -height;                   // negative = top-down DIB (no flip needed)
    bi.biPlanes        = 1;
    bi.biBitCount      = 32;
    bi.biCompression   = BI_RGB;
    bi.biSizeImage     = imageSize;
    bi.biXPelsPerMeter = 0;
    bi.biYPelsPerMeter = 0;
    bi.biClrUsed       = 0;
    bi.biClrImportant  = 0;

    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[BMP] Cannot open file: %s\n", path);
        return false;
    }

    fwrite(&bf, sizeof(bf), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);

    const uint8_t* row = pixels;
    uint8_t padding[4] = {0, 0, 0, 0};

    for (int y = 0; y < height; ++y) {
        fwrite(row, 1, rowSize, f);
        if (padSize > 0) {
            fwrite(padding, 1, padSize, f);
        }
        row += rowSize;
    }

    fclose(f);
    return true;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
//  Test entry point
// ═══════════════════════════════════════════════════════════════

int main() {
    fprintf(stderr, "============================================\n");
    fprintf(stderr, "  Synapse-X: DXGI ROI Capture Test\n");
    fprintf(stderr, "============================================\n\n");

    // ── 1. Initialize ────────────────────────────────────
    SynapseX::DxgiCapturer capturer;
    constexpr int ROI_W = 640;
    constexpr int ROI_H = 640;

    if (!capturer.Initialize(ROI_W, ROI_H)) {
        fprintf(stderr, "[FATAL] DXGI capturer init failed.\n");
        fprintf(stderr, "  Possible causes: no display, unsupported GPU driver,\n");
        fprintf(stderr, "  or Desktop Duplication not available on this output.\n");
        return 1;
    }

    fprintf(stderr, "[INFO] Display resolution: %dx%d\n",
            capturer.GetOutputWidth(), capturer.GetOutputHeight());
    fprintf(stderr, "[INFO] ROI: %dx%d (center crop)\n", ROI_W, ROI_H);

    // Calculate expected source box for diagnostics
    LONG srcLeft = (capturer.GetOutputWidth()  - ROI_W) / 2;
    LONG srcTop  = (capturer.GetOutputHeight() - ROI_H) / 2;
    fprintf(stderr, "[INFO] Source region: left=%ld top=%ld right=%ld bottom=%ld\n",
            srcLeft, srcTop, srcLeft + ROI_W, srcTop + ROI_H);

    fprintf(stderr, "[INFO] Waiting for a new frame...\n");
    fprintf(stderr, "       (move a window or wiggle the mouse to trigger updates)\n\n");

    // ── 2. Capture loop (wait for a real frame) ──────────
    std::vector<uint8_t> buffer;
    constexpr int kMaxAttempts = 300;
    bool captured = false;

    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        if (capturer.CaptureFrame(buffer)) {
            // Check if frame data is non-zero
            bool allZero = true;
            for (size_t i = 0; i < buffer.size() && allZero; ++i) {
                if (buffer[i] != 0) allZero = false;
            }

            const auto& info = capturer.GetLastFrameInfo();
            fprintf(stderr, "[ATTEMPT %3d] Got frame: %zu bytes | "
                    "LastPresentTime=%lld AccumulatedFrames=%u RectCount=%u "
                    "PtrPos=(%d,%d) Visible=%d Protected=%u AllZero=%s\n",
                    attempt,
                    buffer.size(),
                    static_cast<long long>(info.LastPresentTime.QuadPart),
                    info.AccumulatedFrames,
                    info.TotalMetadataBufferSize,
                    static_cast<int>(info.PointerPosition.Position.x),
                    static_cast<int>(info.PointerPosition.Position.y),
                    info.PointerPosition.Visible,
                    info.ProtectedContentMaskedOut,
                    allZero ? "YES" : "NO");

            if (!allZero) {
                captured = true;
                fprintf(stderr, "[OK] Non-zero frame captured on attempt %d!\n", attempt);
                break;
            }
        }
        Sleep(10);
    }

    if (!captured) {
        fprintf(stderr, "\n[ERROR] All %d captured frames were black (all-zero pixels).\n",
                kMaxAttempts);
        fprintf(stderr, "Troubleshooting:\n");
        fprintf(stderr, "  1. Are you on a remote desktop / virtual display?\n");
        fprintf(stderr, "  2. Is the monitor powered on and attached to this GPU?\n");
        fprintf(stderr, "  3. Try running as a console app on the physical desktop.\n");
        fprintf(stderr, "  4. Check if ProtectedContentMaskedOut was set in the logs above.\n");
        return 1;
    }

    // ── 3. Save BMP ──────────────────────────────────────
    const char* outPath = "test_roi.bmp";
    if (!SaveBgraAsBmp(outPath, buffer.data(), ROI_W, ROI_H)) {
        fprintf(stderr, "[FATAL] BMP write failed.\n");
        return 1;
    }

    fprintf(stderr, "[OK] BMP saved: %s (%dx%d, 32-bit BGRA, %zu bytes)\n",
            outPath, ROI_W, ROI_H, buffer.size());

    // Show first few pixels for manual verification
    if (buffer.size() >= 16) {
        const uint8_t* p = buffer.data();
        fprintf(stderr, "[INFO] First 4 pixels (B,G,R,A): "
                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u] "
                "[%3u,%3u,%3u,%3u] [%3u,%3u,%3u,%3u]\n",
                p[0],p[1],p[2],p[3],   p[4],p[5],p[6],p[7],
                p[8],p[9],p[10],p[11], p[12],p[13],p[14],p[15]);
    }

    // ── 4. LZ4 compression test ──────────────────────────
    fprintf(stderr, "\n============================================\n");
    fprintf(stderr, "  LZ4 Compression Test\n");
    fprintf(stderr, "============================================\n");

    const int rawSize = static_cast<int>(buffer.size());

    // Initialize compressor (pre-allocates internal buffers)
    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        fprintf(stderr, "[FATAL] LZ4 compressor init failed.\n");
        return 1;
    }
    fprintf(stderr, "[INFO] Compressor initialized for max input: %d bytes\n",
            compressor.GetMaxInputSize());
    fprintf(stderr, "[INFO] Worst-case compression buffer: %d bytes (LZ4_compressBound)\n\n",
            SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // Compress and measure time
    std::vector<uint8_t> compressed;
    auto t0 = std::chrono::high_resolution_clock::now();

    if (!compressor.Compress(buffer.data(), rawSize, compressed)) {
        fprintf(stderr, "[FATAL] LZ4 compression failed.\n");
        return 1;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double compressMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Stats
    int compressedSize = static_cast<int>(compressed.size());
    double ratio = (rawSize > 0)
        ? (100.0 * compressedSize / rawSize)
        : 0.0;
    double throughputMBps = (compressMs > 0.0)
        ? (rawSize / (1024.0 * 1024.0)) / (compressMs / 1000.0)
        : 0.0;

    fprintf(stderr, "[LZ4] Compression results:\n");
    fprintf(stderr, "  Raw size:       %10d bytes  (%.2f MB)\n",
            rawSize, rawSize / (1024.0 * 1024.0));
    fprintf(stderr, "  Compressed:     %10d bytes  (%.2f MB)\n",
            compressedSize, compressedSize / (1024.0 * 1024.0));
    fprintf(stderr, "  Ratio:          %10.1f %%  (of original)\n", ratio);
    fprintf(stderr, "  Time:           %10.3f ms\n", compressMs);
    fprintf(stderr, "  Throughput:     %10.1f MB/s\n", throughputMBps);

    fprintf(stderr, "\n[DONE] All tests passed.\n");
    return 0;
}
