#pragma once

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
    HUB_START_OFFLINE_STUDIO
};

struct HubClickResult {
    HubActionType action = HUB_NONE;
    int index = -1;
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
    bool enableNativeVideo(uint32_t phoneW, uint32_t phoneH, std::string& outError);
    void disableNativeVideo();
    bool isNativeVideoActive() const { return nativeVideoActive_ && d3d_.isInitialized(); }
    void submitDecodedFrame(const DecodedNV12Frame& frame);
    void updateEmbeddedHud(const void* bgraPremulPixels, int width, int height);
    void renderNativeFrame(bool drawHudOverlay);
    bool captureNativeScreenshot(const std::string& savePath, std::string& outError) const;
    bool hasNativeVideoFrame() const { return d3d_.hasVideoFrame(); }

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

    // Renders either dark letterbox backdrop (connected) or interactive Device Hub (disconnected)
    void paint(Gdiplus::Graphics& g, const RECT& clientRc,
               bool connected, bool demoMode, bool connecting,
               const std::string& statusMsg,
               const std::vector<AdbDeviceInfo>& devices,
               const std::string& wirelessAddr, bool editingWirelessAddr,
               bool showingQrPair, const QrMatrix* qrMatrix, const std::string& qrStatus,
               int hoverX, int hoverY);

    HubClickResult hitTestHub(int x, int y) const;

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
    bool nativeVideoActive_ = false;
    D3D11Renderer d3d_;
    RECT hostBounds_;
    ViewportRect videoRect_;
    uint32_t lastPhoneW_ = 1920;
    uint32_t lastPhoneH_ = 1080;
    std::vector<HubHotspot> hotspots_;
};
