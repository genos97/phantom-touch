#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "ui/GdiPlusDraw.hpp"

#include <string>
#include <vector>

enum HubActionType {
    HUB_NONE = 0,
    HUB_CONNECT_DEVICE,   // extra = index in devices
    HUB_REFRESH_DEVICES,
    HUB_FOCUS_WIFI_ADDR,
    HUB_CONNECT_WIFI,
    HUB_START_OFFLINE_STUDIO
};

struct HubClickResult {
    HubActionType action = HUB_NONE;
    int index = -1;
};

// Manages the center viewport of the Emulator Shell:
//   - When connected: owns the SetParent-embedded borderless scrcpy.exe HWND,
//     maintains exact phone aspect-ratio letterboxing (auto-rotation aware),
//     and cleanly detaches before scrcpy exits so the shell never deadlocks.
//   - When disconnected: renders an interactive Device Hub (USB list, Wireless ADB,
//     and Offline Keymap Studio launcher) inside the center canvas.
class ViewportHost {
public:
    ViewportHost();
    ~ViewportHost();

    bool create(HWND parentHwnd, HINSTANCE hInst);
    void setBounds(const RECT& boundsInParent);
    HWND hwnd() const { return hwnd_; }

    // Attempts to locate & embed the launched borderless scrcpy window by title + PID
    bool tryEmbedMirror(const std::string& windowTitle, DWORD expectedPid,
                        uint32_t phoneW, uint32_t phoneH);
    bool isMirrorEmbedded() const { return embeddedHwnd_ != NULL && IsWindow(embeddedHwnd_); }
    HWND embeddedHwnd() const { return embeddedHwnd_; }

    // Updates letterboxed video rect when Shell resizes or phone rotates (portrait <-> landscape)
    void updateVideoLayout(uint32_t phoneW, uint32_t phoneH);

    // Cleanly detaches embedded scrcpy window BEFORE terminating scrcpy.exe
    void detachMirror();

    // Returns the active phone display rect in screen coordinates (for pinning HudOverlay)
    RECT activeVideoScreenRect() const;
    ViewportRect activeVideoClientRect() const { return videoRect_; }

    // Renders either dark letterbox backdrop (connected) or interactive Device Hub (disconnected)
    void paint(Gdiplus::Graphics& g, const RECT& clientRc,
               bool connected, bool demoMode, bool connecting,
               const std::string& statusMsg,
               const std::vector<AdbDeviceInfo>& devices,
               const std::string& wirelessAddr, bool editingWirelessAddr,
               int hoverX, int hoverY);

    HubClickResult hitTestHub(int x, int y) const;

private:
    static LRESULT CALLBACK hostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    struct HubHotspot {
        RECT rc;
        HubActionType act;
        int extra;
    };

    HWND hwnd_ = NULL;
    HWND parentHwnd_ = NULL;
    HWND embeddedHwnd_ = NULL;
    RECT hostBounds_;
    ViewportRect videoRect_;
    uint32_t lastPhoneW_ = 1920;
    uint32_t lastPhoneH_ = 1080;
    std::vector<HubHotspot> hotspots_;
};
