// ─── DxgiCapturer.cpp ──────────────────────────────────────────
// DXGI Desktop Duplication capture module
//
// Data flow (all GPU-side where possible):
//   Desktop Frame (VRAM)
//        |
//        v CopySubresourceRegion (ROI only)
//   Staging Texture (VRAM, CPU_ACCESS_READ)
//        |
//        v Map / memcpy row-by-row / Unmap
//   std::vector<uint8_t> (System RAM, BGRA contiguous)

#include "DxgiCapturer.h"

#include <cstring>
#include <cstdio>

// Always-on diagnostic logging (use SX_LOG_ALWAYS for critical info)
#define SX_LOG(fmt, ...) fprintf(stderr, "[DxgiCapturer] " fmt "\n", ##__VA_ARGS__)

namespace SynapseX {

// ═══════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════

DxgiCapturer::~DxgiCapturer() {
    Cleanup();
}

// ═══════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════

bool DxgiCapturer::Initialize(int roiWidth, int roiHeight) {
    if (m_initialized) {
        Cleanup();
    }

    m_roiWidth  = roiWidth;
    m_roiHeight = roiHeight;

    if (!CreateDeviceAndDuplication()) {
        SX_LOG("Init FAILED: CreateDeviceAndDuplication() returned false");
        Cleanup();
        return false;
    }

    m_initialized = true;
    SX_LOG("Init OK. Output: %dx%d, ROI: %dx%d (center-crop)",
           m_outputWidth, m_outputHeight, m_roiWidth, m_roiHeight);
    return true;
}

bool DxgiCapturer::CaptureFrame(std::vector<uint8_t>& outBuffer) {
    if (!m_initialized || !m_duplication) {
        return false;
    }

    if (m_recreating) {
        return false;
    }

    // ── 1. Acquire desktop frame ──────────────────────────
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};
    Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;

    HRESULT hr = m_duplication->AcquireNextFrame(
        kAcquireTimeoutMs,
        &frameInfo,
        desktopResource.GetAddressOf()
    );

    // No new frame — normal, return false
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }

    // Fullscreen exclusive lost / mode switch — auto-rebuild
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        SX_LOG("ACCESS_LOST detected, rebuilding duplication pipeline...");
        m_recreating = true;
        ReleaseResources();

        if (CreateDeviceAndDuplication()) {
            SX_LOG("Rebuild OK.");
            m_recreating = false;
        } else {
            SX_LOG("Rebuild FAILED, will retry on next call.");
            m_initialized = false;
            m_recreating = false;
        }
        return false;
    }

    if (FAILED(hr)) {
        SX_LOG("AcquireNextFrame FAILED: HRESULT=0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    m_lastFrameInfo = frameInfo;  // cache for diagnostics

    // ── 2. Query desktop texture ──────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
    hr = desktopResource.As(&desktopTexture);
    if (FAILED(hr)) {
        SX_LOG("QI -> ID3D11Texture2D FAILED: 0x%08X", static_cast<unsigned>(hr));
        m_duplication->ReleaseFrame();
        return false;
    }

    // ── 3. GPU copy: desktop ROI -> staging texture ───────
    LONG srcLeft = (static_cast<LONG>(m_outputWidth)  - static_cast<LONG>(m_roiWidth))  / 2;
    LONG srcTop  = (static_cast<LONG>(m_outputHeight) - static_cast<LONG>(m_roiHeight)) / 2;
    if (srcLeft < 0) srcLeft = 0;
    if (srcTop  < 0) srcTop  = 0;

    D3D11_BOX srcBox = {};
    srcBox.left   = static_cast<UINT>(srcLeft);
    srcBox.top    = static_cast<UINT>(srcTop);
    srcBox.right  = srcBox.left + static_cast<UINT>(m_roiWidth);
    srcBox.bottom = srcBox.top  + static_cast<UINT>(m_roiHeight);
    srcBox.front  = 0;
    srcBox.back   = 1;

    if (srcBox.right  > static_cast<UINT>(m_outputWidth))  srcBox.right  = m_outputWidth;
    if (srcBox.bottom > static_cast<UINT>(m_outputHeight)) srcBox.bottom = m_outputHeight;

    m_context->CopySubresourceRegion(
        m_stagingTexture.Get(),   // dst
        0, 0, 0, 0,              // DstSubresource, DstX,Y,Z
        desktopTexture.Get(),     // src
        0,                        // SrcSubresource
        &srcBox                   // src region (ROI only)
    );

    // Flush GPU pipeline to ensure CopySubresourceRegion completes
    // before we ReleaseFrame (which may invalidate the desktop texture).
    m_context->Flush();

    // ── 4. Release desktop frame ──────────────────────────
    m_duplication->ReleaseFrame();

    // ── 5. Map staging texture -> copy row-by-row ─────────
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = m_context->Map(m_stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        SX_LOG("Map staging texture FAILED: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    UINT rowSize = m_roiWidth * kBytesPerPixel;
    outBuffer.resize(static_cast<size_t>(m_roiHeight) * rowSize);

    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t*       dst = outBuffer.data();

    for (int row = 0; row < m_roiHeight; ++row) {
        std::memcpy(
            dst + static_cast<size_t>(row) * rowSize,
            src + static_cast<size_t>(row) * mapped.RowPitch,
            rowSize
        );
    }

    m_context->Unmap(m_stagingTexture.Get(), 0);

    return true;
}

void DxgiCapturer::Cleanup() {
    ReleaseResources();
    m_initialized   = false;
    m_outputWidth   = 0;
    m_outputHeight  = 0;
}

// ═══════════════════════════════════════════════════════════════
//  Private: Device / Duplication creation
// ═══════════════════════════════════════════════════════════════

bool DxgiCapturer::CreateDeviceAndDuplication() {
    // ── Step 1: Create D3D11 device ──────────────────────
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(
        nullptr,                              // pAdapter (default GPU)
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        m_device.GetAddressOf(),
        nullptr,                              // pFeatureLevel
        m_context.GetAddressOf()
    );

    if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        SX_LOG("D3D11 debug layer unavailable, falling back to release mode");
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            createFlags, featureLevels, ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            m_device.GetAddressOf(), nullptr, m_context.GetAddressOf()
        );
    }

    if (FAILED(hr)) {
        SX_LOG("D3D11CreateDevice FAILED: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // ── Step 2: D3D11Device -> IDXGIDevice -> IDXGIAdapter
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = m_device.As(&dxgiDevice);
    if (FAILED(hr)) {
        SX_LOG("QI -> IDXGIDevice FAILED: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) {
        SX_LOG("GetAdapter FAILED: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // Log adapter description for multi-GPU debugging
    DXGI_ADAPTER_DESC adapterDesc = {};
    adapter->GetDesc(&adapterDesc);
    SX_LOG("Adapter: %ls (vendorId=0x%04X, deviceId=0x%04X)",
           adapterDesc.Description,
           adapterDesc.VendorId, adapterDesc.DeviceId);

    // ── Step 3: Adapter -> enumerate Output -> IDXGIOutput1
    // Enumerate all outputs to find the active one
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    int outputIndex = 0;
    bool foundOutput = false;

    for (; outputIndex < 16; ++outputIndex) {
        hr = adapter->EnumOutputs(outputIndex, output.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) continue;

        DXGI_OUTPUT_DESC desc = {};
        hr = output->GetDesc(&desc);
        if (FAILED(hr)) continue;

        // Check if this output has a non-zero desktop area
        int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        SX_LOG("Output[%d]: %ls (%dx%d at %ld,%ld)%s",
               outputIndex,
               desc.DeviceName,
               w, h,
               static_cast<long>(desc.DesktopCoordinates.left),
               static_cast<long>(desc.DesktopCoordinates.top),
               desc.AttachedToDesktop ? " [attached]" : "");

        if (desc.AttachedToDesktop && w > 0 && h > 0) {
            m_outputWidth  = w;
            m_outputHeight = h;
            foundOutput = true;
            break;  // Use first attached desktop output
        }
    }

    if (!foundOutput) {
        SX_LOG("No attached desktop output found among %d outputs", outputIndex);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
    hr = output.As(&output1);
    if (FAILED(hr)) {
        SX_LOG("QI -> IDXGIOutput1 FAILED: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // ── Step 4: Create desktop duplication interface ─────
    hr = output1->DuplicateOutput(m_device.Get(), m_duplication.GetAddressOf());
    if (hr == DXGI_ERROR_UNSUPPORTED) {
        SX_LOG("DuplicateOutput: DXGI_ERROR_UNSUPPORTED — "
               "driver or display does not support Desktop Duplication");
        return false;
    }
    if (FAILED(hr)) {
        SX_LOG("DuplicateOutput FAILED: 0x%08X", static_cast<unsigned>(hr));
        return false;
    }

    // ── Step 5: Create staging texture (ROI size, CPU-readable)
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width              = static_cast<UINT>(m_roiWidth);
    stagingDesc.Height             = static_cast<UINT>(m_roiHeight);
    stagingDesc.MipLevels          = 1;
    stagingDesc.ArraySize          = 1;
    stagingDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count   = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage              = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags          = 0;
    stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags          = 0;

    hr = m_device->CreateTexture2D(&stagingDesc, nullptr, m_stagingTexture.GetAddressOf());
    if (FAILED(hr)) {
        SX_LOG("CreateTexture2D (staging, %dx%d) FAILED: 0x%08X",
               m_roiWidth, m_roiHeight, static_cast<unsigned>(hr));
        return false;
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════
//  Private: Resource release
// ═══════════════════════════════════════════════════════════════

void DxgiCapturer::ReleaseResources() {
    m_stagingTexture.Reset();
    m_duplication.Reset();
    m_context.Reset();
    m_device.Reset();
}

} // namespace SynapseX
