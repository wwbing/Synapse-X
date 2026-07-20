// ─── Synapse-X 主机端 -- 生产主循环 ─────────────────
//
// 固定的 170 Hz 管线：
//   DXGI 采集（每个 tick 尝试）
//     -> 新帧？LZ4 压缩 + 更新缓存
//     -> 无变化？重新发送缓存的压缩帧
//   UDP 分片与发送（每个 tick）
//   UDP 回复接收（每次发送后排空）
//
// 用法：
//   SynapseX_Host.exe [目标IP] [端口] [roi宽] [roi高]
//   默认值：192.168.100.2  8888   640     640

#include "DxgiCapturer.h"
#include "Log.h"
#include "Lz4Compressor.h"
#include "UdpSender.h"
#include "UdpReplyReceiver.h"
#include "MouseController.h"
#include "HttpTuner.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <atomic>
#include <thread>

#include <windows.h>    // timeBeginPeriod / timeEndPeriod
#include <mmsystem.h>

static std::atomic<bool> g_running{true};

using Clock     = std::chrono::high_resolution_clock;
using TimePoint = Clock::time_point;

static inline double ToMs(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

struct PerfStats {
    int    captured    = 0;    // 本窗口内来自 DXGI 的新帧数
    int    sent        = 0;    // 本窗口内 UDP 发送总数
    double sumCapture  = 0.0;  // 仅在新帧上测量
    double sumCompress = 0.0;
    double sumSend     = 0.0;

    void reset() {
        captured    = 0;
        sent        = 0;
        sumCapture  = 0.0;
        sumCompress = 0.0;
        sumSend     = 0.0;
    }
};

// ── 中间瞄准点（跨所有模型统一）──
struct AimPoint {
    float cx, cy;       // 屏幕空间目标坐标
    int   priority;     // 1 = 主目标（真实头部/单类），2 = 备选目标（身体模拟头部）
    float distance;     // 距屏幕中心的像素距离
};

int main(int argc, char* argv[]) {
    // 控制台输出设为 UTF-8 编码，防止中文乱码
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SynapseX::Log::Initialize("host");

    // ── 解析参数 ──────────────────────────────────
    const char* targetIp   = (argc > 1) ? argv[1] : "192.168.100.2";
    uint16_t    targetPort = (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : 8888;
    int         roiW       = (argc > 3) ? std::atoi(argv[3]) : 416;
    int         roiH       = (argc > 4) ? std::atoi(argv[4]) : 416;

    if (roiW < 64 || roiH < 64 || roiW > 4096 || roiH > 4096) {
        SX_LOG_CRITICAL("[Host] 无效的 ROI {}x{}（允许范围: 64..4096）", roiW, roiH);
        return 1;
    }

    int rawSize = roiW * roiH * 4;

    // ── 固定的 170 Hz 节奏 ──────────────────────────────
    constexpr double kTargetFps  = 170.0;
    constexpr double kTargetMs   = 1000.0 / kTargetFps;   // ~5.882 毫秒
    const auto       kInterval   = std::chrono::duration<double, std::milli>(kTargetMs);

    // 提升 Windows 定时器分辨率。
    // 默认是 15.6 毫秒 -- 对于 5.88 毫秒的 tick 来说太粗糙了。
    timeBeginPeriod(1);

    // 将主线程固定到指定的 P 核，防止 OS 调度器
    // 将其在不同核心间迁移 -- 避免 L1/L2 缓存抖动。
    DWORD_PTR affinityMask = 1ULL << 2;  // 核心 2（根据 CPU 拓扑调整）
    if (!SetThreadAffinityMask(GetCurrentThread(), affinityMask)) {
        SX_LOG_WARN("[Host] SetThreadAffinityMask 失败 (error={})",
                    static_cast<unsigned long>(GetLastError()));
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SX_LOG_INFO("[Host] 主线程已绑定到核心2，TIME_CRITICAL 优先级");
    SX_LOG_INFO("[Host] 启动固定频率管线: target={}Hz target={} target_port={} roi={}x{} raw_mb={:.2f} frame_budget_ms={:.3f}",
                kTargetFps,
                targetIp,
                targetPort,
                roiW,
                roiH,
                rawSize / (1024.0 * 1024.0),
                kTargetMs);

    // ── 阶段 0：初始化所有模块 ────────────────────────
    SynapseX::DxgiCapturer capturer;
    if (!capturer.Initialize(roiW, roiH)) {
        SX_LOG_CRITICAL("[Host] DxgiCapturer 初始化失败");
        return 1;
    }

    SynapseX::Lz4Compressor compressor;
    if (!compressor.Initialize(rawSize)) {
        SX_LOG_CRITICAL("[Host] Lz4Compressor 初始化失败");
        return 1;
    }

    SynapseX::UdpSender sender;
    if (!sender.Initialize(targetIp, targetPort)) {
        SX_LOG_CRITICAL("[Host] UdpSender 初始化失败");
        return 1;
    }

    // UDP 回复接收器（客户端 -> 主机端，端口 8889）
    SynapseX::UdpReplyReceiver replyReceiver;
    if (!replyReceiver.Initialize(8889)) {
        SX_LOG_CRITICAL("[Host] UdpReplyReceiver 初始化失败");
        return 1;
    }
    replyReceiver.SetRoiParams(roiW, roiH,
                                capturer.GetOutputWidth(),
                                capturer.GetOutputHeight());

    // 鼠标控制器（ddll64.dll 用于辅助瞄准）
    SynapseX::MouseController mouse;

    if (!mouse.Load("ddll64.dll")) {
        SX_LOG_WARN("[Host] MouseController 不可用；自瞄已禁用。请检查 ddll64.dll 是否在可执行文件旁边");
    }

    // Web 调参面板（在后台线程中运行）
    SynapseX::HttpTuner tuner;
    if (!tuner.Start(9999)) {
        SX_LOG_WARN("[Host] HttpTuner 启动失败；网页控制面板不可用");
    }

    int screenW = capturer.GetOutputWidth();
    int screenH = capturer.GetOutputHeight();

    SX_LOG_INFO("[Host] 所有模块已初始化；进入主循环");
    SX_LOG_INFO("[Host] 如果游戏中采集失败，请尝试无边框窗口模式");
    SX_LOG_INFO("[Host] 按 Ctrl+C 停止");

    // ── 诊断计数器 ──────────────────────────────
    int      zeroFrameCount = 0;
    bool     warnedProtected = false;
    bool     warnedZero = false;

    // ── 主循环状态 ──────────────────────────────────
    std::vector<uint8_t> rawBuffer;
    rawBuffer.reserve(rawSize);

    std::vector<uint8_t> compressedBuffer;
    compressedBuffer.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));

    // 缓存的压缩帧 -- 桌面空闲时重新发送。
    std::vector<uint8_t> cachedCompressed;
    cachedCompressed.reserve(SynapseX::Lz4Compressor::GetMaxOutputSize(rawSize));
    bool hasCachedFrame = false;

    uint32_t    frameId       = 0;
    PerfStats   stats;
    TimePoint   windowStart   = Clock::now();
    int64_t     totalSent     = 0;
    TimePoint   sessionStart  = Clock::now();

    const uint16_t roiW16 = static_cast<uint16_t>(roiW);
    const uint16_t roiH16 = static_cast<uint16_t>(roiH);

    // ── 空间锁定状态（优先级感知）────────────────
    constexpr float kKeepLockRadius = 80.0f;   // 像素 -- 保持锁定的最大距离
    constexpr int   kMaxLostFrames  = 5;       // 放弃前的最大丢失帧数

    bool  isLocked        = false;
    float lockedTargetX   = 0.0f;
    float lockedTargetY   = 0.0f;
    int   lostFrames      = 0;
    int   lockedPriority  = 0;  // 1=主目标, 2=备选目标

    auto nextTick = Clock::now();

    // ═══════════════════════════════════════════════════════
    //  主循环 -- 固定 170 Hz
    // ═══════════════════════════════════════════════════════
    while (g_running) {
        // ── 热键：PageUp=开启瞄准, PageDown=关闭瞄准 ──────────────
        static bool pgupWasDown = false, pgdnWasDown = false;
        bool pgupDown = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0;
        bool pgdnDown = (GetAsyncKeyState(VK_NEXT)  & 0x8000) != 0;
        if (pgupDown && !pgupWasDown) {
            if (!tuner.IsAimEnabled()) {
                tuner.SetAimEnabled(true);
                SX_LOG_INFO("[Host] 自瞄已通过热键启用");
            }
        }
        if (pgdnDown && !pgdnWasDown) {
            if (tuner.IsAimEnabled()) {
                tuner.SetAimEnabled(false);
                SX_LOG_INFO("[Host] 自瞄已通过热键禁用");
            }
        }
        pgupWasDown = pgupDown;
        pgdnWasDown = pgdnDown;

        // ── 阶段 1：采集（每个 tick 尝试）────────────
        auto t0 = Clock::now();
        bool gotFrame = capturer.CaptureFrame(rawBuffer);
        auto t1 = Clock::now();

        if (gotFrame) {
            // ── 诊断：检测全零帧 / 受保护帧 ──
            const auto& fi = capturer.GetLastFrameInfo();
            if (fi.ProtectedContentMaskedOut && !warnedProtected) {
                SX_LOG_WARN("[Host] 检测到受保护内容已屏蔽；DRM或反作弊可能正在阻止采集。请尝试以无边框窗口模式运行游戏");
                warnedProtected = true;
            }

            bool allZero = true;
            for (size_t k = 0; k < rawBuffer.size(); ++k) {
                if (rawBuffer[k] != 0) { allZero = false; break; }
            }
            if (allZero) {
                zeroFrameCount++;
                if (!warnedZero && zeroFrameCount > 10) {
                    SX_LOG_WARN("[Host] 检测到连续{}个零帧：屏幕={}x{} ROI={}x{} 受保护={}。独占全屏可能正在阻止采集",
                                zeroFrameCount,
                                screenW,
                                screenH,
                                roiW,
                                roiH,
                                fi.ProtectedContentMaskedOut);
                    warnedZero = true;
                }
            } else {
                zeroFrameCount = 0;
                warnedZero = false;
            }

            // ── 阶段 2：压缩（仅在新内容时执行）───
            auto t2a = Clock::now();
            bool ok = compressor.Compress(rawBuffer.data(),
                                           static_cast<int>(rawBuffer.size()),
                                           compressedBuffer);
            auto t2b = Clock::now();

            if (ok) {
                // 更新缓存以备后续重新发送
                cachedCompressed = compressedBuffer;
                hasCachedFrame   = true;

                stats.captured++;
                stats.sumCapture  += ToMs(t1  - t0);
                stats.sumCompress += ToMs(t2b - t2a);
            }
        }

        // ── 阶段 3：发送（始终执行 -- 空闲时复用缓存）──
        if (hasCachedFrame) {
            auto t3a = Clock::now();
            bool sent = sender.SendCompressedFrame(
                cachedCompressed.data(),
                static_cast<uint32_t>(cachedCompressed.size()),
                frameId,
                roiW16, roiH16,
                tuner.GetConfig().modelId);
            auto t3b = Clock::now();

            if (sent) totalSent++;

            stats.sent++;
            stats.sumSend += ToMs(t3b - t3a);
            frameId++;
        }

        // ── 阶段 4：接收推理回复 ────────────
        {
            std::vector<SynapseX::Detection> detections;
            uint32_t replyFrameId = 0;
            if (replyReceiver.ReceiveReplies(detections, replyFrameId)) {
                if (!detections.empty()) {
                    auto aimCfg = tuner.GetConfig();
                    uint8_t modelId = aimCfg.modelId;

                    static uint8_t lastModelId = 0xFF;
                    if (modelId != lastModelId) {
                        SX_LOG_INFO("[Host] 当前模型已切换到 id={}",
                                    static_cast<unsigned>(modelId));
                        lastModelId = modelId;
                    }

                    // ── 数据归一化：所有模型 → 统一的 AimPoint[] ──
                    float scrCx = static_cast<float>(screenW) * 0.5f;
                    float scrCy = static_cast<float>(screenH) * 0.5f;

                    std::vector<AimPoint> aimPoints;
                    aimPoints.reserve(detections.size() * 2); // Delta 可能加倍

                    for (const auto& d : detections) {
                        float bw = d.x2 - d.x1;
                        float bh = d.y2 - d.y1;
                        float bcx = (d.x1 + d.x2) * 0.5f;
                        float bcyCenter = (d.y1 + d.y2) * 0.5f;
                        float bcyHead   = d.y1 + bh * aimCfg.headOffset;

                        AimPoint ap;
                        ap.cx = bcx;

                        switch (modelId) {
                        case 0: // Apex(新) -- 2类：0=队友, 1=敌人（保留classId=1）
                            if (d.classId == 1 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;

                        case 3: // OW2       -- 1类：classId 0 = 敌人
                        case 4: // Aimlabs   -- 1类：classId 0 = 敌人
                            if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;

                        case 1: // Delta     -- 2类：0=身体, 1=头部
                        case 5: // PUBG      -- 2类：0=身体, 1=头部
                        case 6: // CrossFire -- 2类：0=身体, 1=头部（同Delta）
                            if (aimCfg.aimPoint == 1) {
                                if (d.classId == 1 && d.confidence >= aimCfg.deltaHeadConfidence) {
                                    ap.cy       = bcyCenter;
                                    ap.priority = 1;
                                } else if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                    ap.cy       = bcyHead;   // 身体 → 模拟头部（已过滤）
                                    ap.priority = 2;
                                } else { break; }
                            } else {
                                // 身体模式：仅身体，按置信度过滤
                                if (d.classId != 0 || d.confidence < aimCfg.minConfidence) break;
                                ap.cy       = bcyCenter;
                                ap.priority = 1;
                            }
                            ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                            aimPoints.push_back(ap);
                            break;

                        case 2: // BF6       -- 2类：0=敌人, 1=队友（丢弃）
                            if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;

                        case 7: // CS2       -- 2类：0=CT, 1=T（同BF6风格，支持classFilter）
                            if (aimCfg.classFilter != 2 && d.classId != static_cast<uint32_t>(aimCfg.classFilter))
                                break;
                            if (d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;

                        default:
                            if (d.classId == 0 && d.confidence >= aimCfg.minConfidence) {
                                ap.cy       = (aimCfg.aimPoint == 1) ? bcyHead : bcyCenter;
                                ap.priority = 1;
                                ap.distance = std::sqrt((bcx-scrCx)*(bcx-scrCx) + (ap.cy-scrCy)*(ap.cy-scrCy));
                                aimPoints.push_back(ap);
                            }
                            break;
                        }
                    }

                    if (!aimPoints.empty()) {
                        const AimPoint* best = nullptr;
                        float bestDist = 1e9f;

                        if (isLocked) {
                            // ── 阶段 A：维持锁定 ──────────────
                            const AimPoint* bestPri1 = nullptr;
                            const AimPoint* bestPri2 = nullptr;
                            float dPri1 = 1e9f, dPri2 = 1e9f;

                            for (const auto& ap : aimPoints) {
                                float d = std::sqrt(
                                    (ap.cx - lockedTargetX) * (ap.cx - lockedTargetX) +
                                    (ap.cy - lockedTargetY) * (ap.cy - lockedTargetY));
                                if (d < kKeepLockRadius) {
                                    if (ap.priority == 1 && d < dPri1) {
                                        dPri1 = d; bestPri1 = &ap;
                                    } else if (ap.priority == 2 && d < dPri2) {
                                        dPri2 = d; bestPri2 = &ap;
                                    }
                                }
                            }

                            if (bestPri1) {
                                best = bestPri1;
                                lockedPriority = 1;
                                lostFrames = 0;
                            } else if (bestPri2) {
                                best = bestPri2;
                                lockedPriority = 2;
                                lostFrames = 0;
                            } else {
                                lostFrames++;
                                if (lostFrames > kMaxLostFrames) {
                                    isLocked = false;
                                    lostFrames = 0;
                                    lockedPriority = 0;
                                }
                            }
                        } else {
                            // ── 阶段 B：获取锁定 ───────────────
                            const AimPoint* bestPri1 = nullptr;
                            const AimPoint* bestPri2 = nullptr;
                            float dPri1 = 1e9f, dPri2 = 1e9f;

                            for (const auto& ap : aimPoints) {
                                if (ap.distance > aimCfg.aimRange) continue;
                                if (ap.priority == 1 && ap.distance < dPri1) {
                                    dPri1 = ap.distance; bestPri1 = &ap;
                                } else if (ap.priority == 2 && ap.distance < dPri2) {
                                    dPri2 = ap.distance; bestPri2 = &ap;
                                }
                            }

                            if (bestPri1) {
                                best = bestPri1;
                                lockedPriority = 1;
                                isLocked = true;
                                lostFrames = 0;
                            } else if (bestPri2) {
                                best = bestPri2;
                                lockedPriority = 2;
                                isLocked = true;
                                lostFrames = 0;
                            }
                        }

                        if (best) {
                            lockedTargetX = best->cx;
                            lockedTargetY = best->cy;

                            float autoScaleX = (screenW > 0)
                                ? static_cast<float>(aimCfg.gameW) / static_cast<float>(screenW) : 1.0f;
                            float autoScaleY = (screenH > 0)
                                ? static_cast<float>(aimCfg.gameH) / static_cast<float>(screenH) : 1.0f;

                            float dx = (best->cx - scrCx) * autoScaleX;
                            float dy = (best->cy - scrCy) * autoScaleY;

                            bestDist = best->distance;

                            tuner.UpdateTarget(best->cx, best->cy,
                                               1.0f, bestDist, 0);

                            mouse.SetConfig(aimCfg);

                            if (tuner.IsAimEnabled() &&
                                mouse.AimAtTarget(dx, dy, 1.0f,
                                                  screenW, screenH, aimCfg)) {
                                static int aimCount = 0;
                                if (++aimCount % 30 == 1) {
                                    SX_LOG_DEBUG("[Host] 瞄准目标 x={:.0f} y={:.0f} 优先级={} 距离={:.0f}",
                                                 static_cast<double>(best->cx),
                                                 static_cast<double>(best->cy),
                                                 best->priority,
                                                 static_cast<double>(bestDist));
                                }
                            }
                        }
                    }
                }
            }
        }
        // ── 每秒统计报告 ──────────────────────
        double elapsed = ToMs(Clock::now() - windowStart) / 1000.0;
        if (elapsed >= 1.0) {
            double sendFps    = stats.sent / elapsed;
            double captureFps = stats.captured / elapsed;
            double avgCapture = stats.captured > 0 ? stats.sumCapture  / stats.captured : 0.0;
            double avgCompress= stats.captured > 0 ? stats.sumCompress / stats.captured : 0.0;
            double avgSend    = stats.sent     > 0 ? stats.sumSend     / stats.sent     : 0.0;

            double pipelineTotal = avgCapture + avgCompress + avgSend;

            SX_LOG_DEBUG("[Host] 统计：发送帧率={:.1f} 采集帧率={:.1f} 新帧数={} 缓存帧数={} 总发送数={} 采集耗时={:.3f} 压缩耗时={:.3f} 发送耗时={:.3f} 管线耗时={:.3f} 预算={:.3f}",
                         sendFps,
                         captureFps,
                         stats.captured,
                         stats.sent - stats.captured,
                         static_cast<long long>(totalSent),
                         avgCapture,
                         avgCompress,
                         avgSend,
                         pipelineTotal,
                         kTargetMs);

            // 推送到 Web 调参面板
            tuner.UpdateStats(sendFps, captureFps,
                              pipelineTotal, avgCompress,
                              stats.captured, stats.sent - stats.captured,
                              static_cast<uint64_t>(totalSent));

            stats.reset();
            windowStart = Clock::now();
        }

        // ── 维持 170 Hz 节奏 ──────────────────────
        nextTick += std::chrono::duration_cast<Clock::duration>(kInterval);
        auto now = Clock::now();
        if (nextTick > now) {
            std::this_thread::sleep_until(nextTick);
        } else {
            // 落后于计划 -- 重置以避免死亡螺旋
            nextTick = now;
        }
    }

    timeEndPeriod(1);

    double sessionSec = ToMs(Clock::now() - sessionStart) / 1000.0;
    SX_LOG_INFO("[Host] 会话已结束：时长={:.1f}秒 总发送数={} 平均帧率={:.1f}",
                sessionSec,
                static_cast<long long>(totalSent),
                sessionSec > 0.0 ? totalSent / sessionSec : 0.0);

    capturer.Cleanup();
    sender.Cleanup();
    replyReceiver.Cleanup();
    mouse.Unload();
    tuner.Stop();
    return 0;
}
