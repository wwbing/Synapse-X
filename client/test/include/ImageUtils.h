#pragma once
// WIC-based JPEG/PNG/BMP loader + BGRA output. Zero external dependencies.

#include <cstdint>
#include <string>
#include <vector>
#include <wincodec.h>
#include <windows.h>

namespace TestUtils {

// Load image, resize to targetW×targetH, output as BGRA uint8.
inline bool LoadAndResize(const wchar_t* path, int targetW, int targetH,
                          std::vector<uint8_t>& outBgra) {
    // COM init
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr) || hr == S_FALSE || hr == RPC_E_CHANGED_MODE;

    IWICImagingFactory* factory = nullptr;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                          CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { if (comInit) CoUninitialize(); return false; }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) { factory->Release(); if (comInit) CoUninitialize(); return false; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    decoder->Release();
    if (FAILED(hr)) { factory->Release(); if (comInit) CoUninitialize(); return false; }

    // Resize
    IWICBitmapScaler* scaler = nullptr;
    hr = factory->CreateBitmapScaler(&scaler);
    if (FAILED(hr)) { frame->Release(); factory->Release(); if (comInit) CoUninitialize(); return false; }

    scaler->Initialize(frame, targetW, targetH, WICBitmapInterpolationModeFant);
    frame->Release();

    // Convert to BGRA8
    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { scaler->Release(); factory->Release(); if (comInit) CoUninitialize(); return false; }

    converter->Initialize(scaler, GUID_WICPixelFormat32bppBGRA,
                          WICBitmapDitherTypeNone, nullptr, 0.0f,
                          WICBitmapPaletteTypeCustom);
    scaler->Release();

    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    UINT stride = w * 4;
    outBgra.resize(static_cast<size_t>(w) * h * 4);
    hr = converter->CopyPixels(nullptr, stride,
                               static_cast<UINT>(outBgra.size()),
                               outBgra.data());
    converter->Release();
    factory->Release();
    if (comInit) CoUninitialize();
    return SUCCEEDED(hr) && !outBgra.empty();
}

// Draw rectangles for detections onto BGRA image (in-place).
// dets: float array [x1,y1,x2,y2,classId] repeated numDets times
// Colors: enemy=red, teammate=self=green, head=yellow, body=blue
inline void DrawBoxes(std::vector<uint8_t>& bgra, int w, int h,
                      const float* dets, int numDets,
                      const char** classNames) {
    for (int i = 0; i < numDets; ++i) {
        const float* d = &dets[i * 5];
        int x1 = (int)d[0], y1 = (int)d[1];
        int x2 = (int)d[2], y2 = (int)d[3];
        int cls = (int)d[4];
        if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
        if (x2 >= w) x2 = w - 1; if (y2 >= h) y2 = h - 1;

        // Pick color by class name
        uint8_t r = 255, g = 0, b = 0; // default red
        if (classNames && cls >= 0) {
            const char* cn = classNames[cls];
            if (cn) {
                if (strcmp(cn, "teammate") == 0 || strcmp(cn, "self") == 0) { r = 0; g = 255; b = 0; }
                else if (strcmp(cn, "head") == 0) { r = 255; g = 255; b = 0; }
                else if (strcmp(cn, "body") == 0) { r = 255; g = 0; b = 0; }
            }
        }

        // Draw 3px border rect
        for (int px = 1; px <= 3; ++px) {
            int l = x1 - px, t = y1 - px, r2 = x2 + px, b2 = y2 + px;
            for (int xx = l; xx <= r2; ++xx) {
                if (xx >= 0 && xx < w) {
                    if (t >= 0 && t < h) { int off = (t * w + xx) * 4; bgra[off]=b; bgra[off+1]=g; bgra[off+2]=r; }
                    if (b2 >= 0 && b2 < h) { int off = (b2 * w + xx) * 4; bgra[off]=b; bgra[off+1]=g; bgra[off+2]=r; }
                }
            }
            for (int yy = t; yy <= b2; ++yy) {
                if (yy >= 0 && yy < h) {
                    if (l >= 0 && l < w) { int off = (yy * w + l) * 4; bgra[off]=b; bgra[off+1]=g; bgra[off+2]=r; }
                    if (r2 >= 0 && r2 < w) { int off = (yy * w + r2) * 4; bgra[off]=b; bgra[off+1]=g; bgra[off+2]=r; }
                }
            }
        }
    }
}

} // namespace TestUtils
