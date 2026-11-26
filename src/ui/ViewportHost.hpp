#pragma once

#include "bridge/ConnectionState.hpp"
#include "bridge/QrCodeGen.hpp"
#include "bridge/ScrcpyBridge.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "video/D3D11Renderer.hpp"

#include <string>
#include <vector>

enum HubActionType {
    HUB_NONE = 0,
    HUB_CONNECT_DEVICE,   // extra = index in devices
    HUB_REFRESH_DEVICES,
    HUB_FOCUS_WIFI_ADDR,
    HUB_CONNECT_WIFI,
    HUB_TOGGLE_QR_PAIR,
    HUB_START_OFFLINE_STUDIO,
    HUB_REPAIR_TOOLS,
    HUB_OPEN_ONBOARDING,
    HUB_CANCEL_RECONNECT,
    HUB_RETRY_RECONNECT,
    HUB_COPY_ERROR
};

struct HubClickResult {
    HubActionType action = HUB_NONE;
    int index = -1;
};

struct HubViewState {
    ConnState connState = CONN_NO_DEVICE;
    ConnectProgressStep connectStep = CONN_STEP_PUSH_SERVER;
    int reconnectCountdownSec = 0;
    int reconnectAttempt = 0;
    int maxReconnectAttempts = 5;
    std::string statusMsg;
    std::string errorDetails;
    bool secureAppBlocked = false;
    const std::vector<AdbDeviceInfo>* devices = nullptr;
    std::string wirelessAddr;
    bool editingWirelessAddr = false;
    bool showingQrPair = false;
    const QrMatrix* qrMatrix = nullptr;
    std::string qrStatus;
};

// Manages the center viewport of the Emulator Shell:
//   - Stage 2 Native Mode: renders phone H.264 video decoded by Media Foundation via Direct3D 11
//     directly inside this child HWND, compositing the keymap HUD in the exact same frame!
//   - Legacy Fallback Mode: embeds borderless scrcpy.exe via SetParent.
//   - Disconnected: renders an interactive Device Hub (USB list, Wireless ADB, QR Pairing,
//     and Offline Keymap Studio launcher).
class ViewportHost {
public:
    ViewportHost();
    ~ViewportHost();

    bool create(HWND parentHwnd, HINSTANCE hInst);
    void setBounds(const RECT& boundsInParent);
    HWND hwnd() const { return hwnd_; }

    // Stage 2 Native Direct3D 11 single-connection renderer
    bool ensureNativeDeviceInitialized(std::string& outError, bool vsyncEnabled = false);
    bool enableNativeVideo(uint32_t phoneW, uint32_t phoneH, std::string& outError, bool vsyncEnabled = false);
    void disableNativeVideo();
    bool isNativeVideoActive() const { return nativeVideoActive_ && d3d_.isInitialized(); }
    ID3D11Device* d3dDevice() const { return d3d_.d3dDevice(); }
    bool supportsGpuNv12() const { return d3d_.supportsGpuNv12(); }
    bool isFlipModel() const { return d3d_.isFlipModel(); }
    bool setVsync(bool enabled) { return d3d_.setVsync(enabled); }
    bool setFullscreenTarget(HWND topLevelHwnd, bool enterFullscreen, int monW, int monH);
    std::string presentationModeLabel() const { return d3d_.presentationModeLabel(); }
    HRESULT lastPresentHResult() const { return d3d_.lastPresentHResult(); }
    bool isOccluded() const { return d3d_.isOccluded(); }
    bool hasRtv() const { return d3d_.hasRtv(); }
    int backbufW() const { return d3d_.backbufW(); }
    bool isNativeDeviceLost() const { return d3d_.isDeviceLost(); }
    bool rebuildNativeDevice(std::string& outError);
    DWORD lastPresentTick() const { return d3d_.lastPresentTick(); }
    bool presentGpuNV12Frame(ID3D11Texture2D* srcTex, uint32_t subresourceIdx,
                             uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                             bool drawHudOverlay, FrameTiming* ioTiming = nullptr);
    void presentCpuNV12Frame(const uint8_t* yPlane, const uint8_t* uvPlane, uint32_t stride,
                             uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                             bool drawHudOverlay, FrameTiming* ioTiming = nullptr);
    void submitDecodedFrame(const DecodedNV12Frame& frame);
    void updateEmbeddedHud(const void* bgraPremulPixels, int width, int height);
    void renderNativeFrame(bool drawHudOverlay, FrameTiming* ioTiming = nullptr);
    bool captureNativeScreenshot(const std::string& savePath, std::string& outError) const;
    bool hasNativeVideoFrame() const { return d3d_.hasVideoFrame(); }

    // Round-trip probe forwarding (LATENCY_PLAN 0.2)
    void setProbeFrameEvent(HANDLE evt) { d3d_.setProbeFrameEvent(evt); }
    void setProbeSampleRect(bool active, float normX, float normY) { d3d_.setProbeSampleRect(active, normX, normY); }
    bool getLastProbeSample(float& outMeanY, int64_t& outPresentQpc, uint64_t& outSeq) const {
        return d3d_.getLastProbeSample(outMeanY, outPresentQpc, outSeq);
    }
    bool sampleGridBrightness(float outGridY[9]) const { return d3d_.sampleGridBrightness(outGridY); }

    // Legacy fallback: locate & embed borderless scrcpy.exe window by title + PID
    bool tryEmbedMirror(const std::string& windowTitle, DWORD expectedPid,
                        uint32_t phoneW, uint32_t phoneH);
    bool isMirrorEmbedded() const { return embeddedHwnd_ != NULL && IsWindow(embeddedHwnd_); }
    HWND embeddedHwnd() const { return embeddedHwnd_; }

    // Updates letterboxed video rect when Shell resizes or phone rotates (portrait <-> landscape)
    void updateVideoLayout(uint32_t phoneW, uint32_t phoneH);

    // Cleanly detaches embedded scrcpy window or shuts down native video view
    void detachMirror();

    // Returns the active phone display rect in screen coordinates (for pinning HudOverlay / mouse hit test)
    RECT activeVideoScreenRect() const;
    ViewportRect activeVideoClientRect() const { return videoRect_; }

    // Renders either dark letterbox backdrop (connected) or interactive X2/F3 Device Hub (disconnected)
    void paint(Gdiplus::Graphics& g, const RECT& clientRc,
               bool connected, bool demoMode,
               const HubViewState& hub,
               int hoverX, int hoverY);

    HubClickResult hitTestHub(int x, int y) const;
    bool onHubMouseWheel(int x, int y, int delta);
    bool hoverHubAt(int x, int y, int& outId, RECT& outRc) const;
    bool rectForHubAction(HubActionType act, int extra, RECT& outRc) const;

private:
    static LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT handleHostMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    struct HubHotspot {
        RECT rc;
        HubActionType act;
        int extra;
    };

    HWND hwnd_ = NULL;
    HWND parentHwnd_ = NULL;
    HWND embeddedHwnd_ = NULL;
    volatile LONG nativeVideoActive_ = 0;
    bool fullscreenTarget_ = false;
    D3D11Renderer d3d_;
    mutable CRITICAL_SECTION rectLock_;
    RECT hostBounds_;
    ViewportRect videoRect_;
    uint32_t lastPhoneW_ = 1920;
    uint32_t lastPhoneH_ = 1080;
    std::vector<HubHotspot> hotspots_;
    RECT hubDeviceListRc_ = { 0, 0, 0, 0 };
    int hubDeviceScrollY_ = 0;
    int hubDeviceMaxScrollY_ = 0;
    Gdiplus::Bitmap* cachedQrBmp_ = nullptr;
    int cachedQrSize_ = 0;
    int cachedQrCellPx_ = 0;
    uint32_t cachedQrHash_ = 0;
};
