// ─── Synapse-X Model Test Tool ────────────────────────────────
// Uses WIC to load JPEG/PNG/BMP, runs TRT inference, saves results.
//
// Usage:
//   .\test_infer.exe <engine_path> <image_path>
//
// Output (in test/result/):
//   <game>_detections.txt   — detection list
//   <game>_boxes.bmp        — annotated image with bounding boxes

#include "../include/ImageUtils.h"
#include "../../include/TrtInference.h"
#include "../../include/CudaPreprocess.h"
#include "../../../shared/include/ReplyPacket.h"

#include <windows.h>
#include <cuda_runtime.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool SaveBgraBmp(const char* path, const uint8_t* px, int w, int h) {
    int rowSize = w * 4, pad = (4 - (rowSize % 4)) % 4;
    BITMAPFILEHEADER bf = {};
    bf.bfType = 0x4D42;
    bf.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + (rowSize + pad) * h;
    bf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1;
    bi.biBitCount = 32; bi.biCompression = BI_RGB;
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(&bf, sizeof(bf), 1, f); fwrite(&bi, sizeof(bi), 1, f);
    uint8_t padBuf[4] = {};
    for (int y = 0; y < h; ++y) {
        fwrite(px + y * rowSize, 1, rowSize, f);
        if (pad) fwrite(padBuf, 1, pad, f);
    }
    fclose(f);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: test_infer.exe <engine> <image>\n");
        fprintf(stderr, "  e.g. test_infer.exe ../model/engine/delta_body_head_416.engine ../image/delta.jpg\n");
        return 1;
    }
    std::string enginePath(argv[1]);
    std::string imagePath(argv[2]);
    fprintf(stderr, "Engine: %s\nImage:  %s\n\n", enginePath.c_str(), imagePath.c_str());

    // ── 1. Load & resize image → 416×416 BGRA ────────────
    int wcharLen = MultiByteToWideChar(CP_UTF8, 0, imagePath.c_str(), -1, nullptr, 0);
    std::vector<wchar_t> wpath(wcharLen);
    MultiByteToWideChar(CP_UTF8, 0, imagePath.c_str(), -1, wpath.data(), wcharLen);

    std::vector<uint8_t> bgra;
    if (!TestUtils::LoadAndResize(wpath.data(), 416, 416, bgra)) {
        fprintf(stderr, "FATAL: Cannot load image: %s\n", imagePath.c_str());
        return 1;
    }
    fprintf(stderr, "Image loaded: %zu bytes (416x416 BGRA)\n", bgra.size());

    // ── 2. GPU init ──────────────────────────────────────
    cudaSetDevice(0);
    SynapseX::TrtInference trt;
    if (!trt.Initialize(enginePath, 416, 416, 300)) {
        fprintf(stderr, "FATAL: TRT init failed\n");
        return 1;
    }
    if (!trt.SetupStream()) {
        fprintf(stderr, "FATAL: Stream setup failed\n");
        return 1;
    }

    // Init GPU preprocess (NVRTC — required before Infer!)
    if (!SynapseX::InitCudaPreprocess()) {
        fprintf(stderr, "FATAL: InitCudaPreprocess failed\n");
        return 1;
    }

    // Warmup
    fprintf(stderr, "Warming up (10 dummy frames)...\n");
    std::vector<uint8_t> black(416 * 416 * 4, 0);
    for (int i = 0; i < 10; ++i) trt.Infer(black.data(), 0.9f);
    cudaDeviceSynchronize();

    // ── 3. Inference ─────────────────────────────────────
    fprintf(stderr, "Running inference...\n");
    auto dets = trt.Infer(bgra.data(), 0.25f);

    // ── 4. Output ────────────────────────────────────────
    // Detect game name from engine filename
    std::string engineName = enginePath;
    size_t slashPos = engineName.find_last_of("/\\");
    if (slashPos != std::string::npos) engineName = engineName.substr(slashPos + 1);

    // Strip prefix to get game name
    // Models: delta_body_head_416, bf6_enemy_self_416, apex_enemy_416, ow2_enemy_416
    std::string gameName = engineName;
    // Remove _416.engine suffix
    size_t pos = gameName.find("_416");
    if (pos != std::string::npos) gameName = gameName.substr(0, pos);

    fprintf(stderr, "Game: %s, Detections: %zu\n\n", gameName.c_str(), dets.size());

    // Class names per model
    const char** classNames = nullptr;
    const char* delta_classes[] = {"body", "head"};
    const char* bf6_classes[]   = {"enemy", "self"};
    const char* apex_classes[]  = {"enemy"};
    const char* ow2_classes[]   = {"enemy"};

    if (gameName.find("delta") != std::string::npos) classNames = delta_classes;
    else if (gameName.find("bf6") != std::string::npos) classNames = bf6_classes;
    else if (gameName.find("apex") != std::string::npos) classNames = apex_classes;
    else if (gameName.find("ow2") != std::string::npos) classNames = ow2_classes;

    // Print all detections
    for (size_t i = 0; i < dets.size(); ++i) {
        const char* cn = "?";
        if (classNames && dets[i].classId >= 0) {
            // Guess: check array
            cn = classNames[dets[i].classId % (gameName.find("delta") != std::string::npos ? 2 :
                                                gameName.find("bf6") != std::string::npos ? 2 : 1)];
        }
        fprintf(stderr, "  [%s] conf=%.3f  box=[%.1f, %.1f, %.1f, %.1f]\n",
                cn, dets[i].confidence,
                dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
    }

    // Save detection text
    char txtPath[512];
    snprintf(txtPath, sizeof(txtPath), "./result/%s_detections.txt", gameName.c_str());
    FILE* tf = fopen(txtPath, "w");
    if (tf) {
        fprintf(tf, "Model: %s\n", engineName.c_str());
        fprintf(tf, "Detections: %zu\n", dets.size());
        for (size_t i = 0; i < dets.size(); ++i) {
            const char* cn = "?";
            if (classNames) {
                int maxCls = gameName.find("delta") != std::string::npos ? 2 :
                             gameName.find("bf6") != std::string::npos ? 2 : 1;
                cn = classNames[dets[i].classId % maxCls];
            }
            fprintf(tf, "%s %.4f %.1f %.1f %.1f %.1f\n",
                    cn, dets[i].confidence,
                    dets[i].x1, dets[i].y1, dets[i].x2, dets[i].y2);
        }
        fclose(tf);
        fprintf(stderr, "\nSaved: %s\n", txtPath);
    }

    // Draw boxes & save BMP (convert Detection to raw arrays)
    if (!dets.empty()) {
        std::vector<float> rawDets; rawDets.reserve(dets.size() * 5);
        for (auto& d : dets) {
            rawDets.push_back(d.x1); rawDets.push_back(d.y1);
            rawDets.push_back(d.x2); rawDets.push_back(d.y2);
            rawDets.push_back((float)d.classId);
        }
        TestUtils::DrawBoxes(bgra, 416, 416, rawDets.data(),
                             static_cast<int>(dets.size()), classNames);
    }

    char bmpPath[512];
    snprintf(bmpPath, sizeof(bmpPath), "./result/%s_boxes.bmp", gameName.c_str());
    if (SaveBgraBmp(bmpPath, bgra.data(), 416, 416)) {
        fprintf(stderr, "Saved: %s\n", bmpPath);
    }

    trt.Cleanup();
    fprintf(stderr, "\nDone.\n");
    return 0;
}
