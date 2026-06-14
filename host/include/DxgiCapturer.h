#pragma once

// ── DXGI 高性能桌面截屏模块 ────────────────────────────────
// 功能：
//   1. 利用 DXGI Desktop Duplication API 零拷贝获取桌面帧
//   2. 通过 GPU 端 CopySubresourceRegion 只提取屏幕中心 ROI
//   3. 通过 Staging Texture 将像素数据回传至系统内存
//
// 技术要点：
//   · 全程 ComPtr 管理 D3D/DXGI 对象生命周期，杜绝泄漏
//   · 自动检测并恢复 DXGI_ERROR_ACCESS_LOST（模式切换/全屏独占丢失）
//   · AcquireNextFrame 超时视为正常情况（无新帧），不抛异常
//   · 输出格式：BGRA (DXGI_FORMAT_B8G8R8A8_UNORM)，每像素 4 字节

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace SynapseX {

// ── 默认 ROI 配置 ─────────────────────────────────────────
constexpr int kDefaultRoiWidth  = 640;
constexpr int kDefaultRoiHeight = 640;
constexpr int kBytesPerPixel    = 4;    // BGRA = 4 bytes

// AcquireNextFrame 超时时间 (毫秒)
// 设为 0 表示立即返回；设为 1 可略微降低 CPU 自旋
constexpr UINT kAcquireTimeoutMs = 0;

class DxgiCapturer {
public:
    DxgiCapturer() = default;
    ~DxgiCapturer();

    // 禁止拷贝/移动 (D3D 对象不适合转移)
    DxgiCapturer(const DxgiCapturer&) = delete;
    DxgiCapturer& operator=(const DxgiCapturer&) = delete;
    DxgiCapturer(DxgiCapturer&&) = delete;
    DxgiCapturer& operator=(DxgiCapturer&&) = delete;

    // ── 生命周期 ──────────────────────────────────────────

    // 初始化 D3D11 设备 + 适配器枚举 + 创建桌面复制接口
    // roiWidth / roiHeight：需要截取的中心区域尺寸
    // 返回 true 表示初始化成功
    bool Initialize(int roiWidth = kDefaultRoiWidth, int roiHeight = kDefaultRoiHeight);

    // 捕获一帧桌面并提取中心 ROI 像素
    // outBuffer：输出缓冲区，会自动 resize 为 ROI 尺寸的 BGRA 数据
    // 返回 true 表示获取到新帧；false 表示无新帧或处于重建中
    bool CaptureFrame(std::vector<uint8_t>& outBuffer);

    // 主动释放所有 GPU 资源（析构时也会自动调用）
    void Cleanup();

    // ── 查询接口 ──────────────────────────────────────────
    int  GetOutputWidth()   const { return m_outputWidth; }
    int  GetOutputHeight()  const { return m_outputHeight; }
    int  GetRoiWidth()      const { return m_roiWidth; }
    int  GetRoiHeight()     const { return m_roiHeight; }
    bool IsInitialized()    const { return m_initialized; }

    // 获取最近一帧的元数据（用于诊断 LastPresentTime、AccumulatedFrames 等）
    const DXGI_OUTDUPL_FRAME_INFO& GetLastFrameInfo() const { return m_lastFrameInfo; }

private:
    // 创建 D3D11Device → DXGI Adapter → Output → Duplication 整条链路
    bool CreateDeviceAndDuplication();

    // 释放所有 ComPtr（用于重建前或析构时）
    void ReleaseResources();

    // ── D3D11 核心对象 ────────────────────────────────────
    Microsoft::WRL::ComPtr<ID3D11Device>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;

    // ── DXGI 桌面复制 ─────────────────────────────────────
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;

    // ── GPU → CPU 中转 Staging 纹理 ───────────────────────
    // · Usage = D3D11_USAGE_STAGING（允许 CPU Read）
    // · 尺寸 = ROI 尺寸（不是全屏！）
    // · 每次捕获前，CopySubresourceRegion 将 ROI 区域拷贝到此纹理
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTexture;

    // ── 状态追踪 ──────────────────────────────────────────
    int  m_roiWidth     = kDefaultRoiWidth;
    int  m_roiHeight    = kDefaultRoiHeight;
    int  m_outputWidth  = 0;
    int  m_outputHeight = 0;
    bool m_initialized  = false;

    // 用于在 ACCESS_LOST 后避免无限递归重建
    bool m_recreating = false;

    // 最近一帧的 DXGI 元数据（用于上层诊断）
    DXGI_OUTDUPL_FRAME_INFO m_lastFrameInfo = {};
};

} // namespace SynapseX
