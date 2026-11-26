#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "config/AppSettings.hpp"
#include "config/Profile.hpp"
#include "ui/GdiPlusDraw.hpp"
#include "video/LatencyTelemetry.hpp"

#include <string>
#include <vector>

struct DrawerActionResult {
    bool profileModified = false;
    bool settingsModified = false;
    bool requestSave = false;
    bool requestCloseDrawer = false;
    bool requestSwitchProfile = false;
    std::string switchProfilePath;
    bool requestAddNode = false;
    KeyActionType addNodeType = NODE_PRESS;
    std::string addNodeLabel;
    bool requestStartRebindNode = false;
    bool requestDeleteSelectedNode = false;
    bool cancelRebind = false;
    bool requestReconnectStream = false;
    bool requestDisconnectDevice = false;
    bool requestConnectWireless = false;
    std::string wirelessAddr;
    bool requestLinkCurrentApp = false;
    bool requestUnlinkApp = false;
    bool requestCalibrateAspect = false;
    bool requestRunLatencyProbe = false;
    bool vsyncToggled = false;
    std::string toastMessage;
};

// Docked 356px Studio Drawer placed OUTSIDE the phone viewport (between ViewportHost and SideRail).
// Supports smooth scrolling, hover states, and all 4 tabs:
//   0 = KEYS (10-control palette + live Key Inspector)
//   1 = SENS (Sensitivity sliders, Aim physics, Fire/WASD/Polling, Lock hotkeys)
//   2 = PROFILES (Create Blank/Clone safely, switch, delete)
//   3 = DEVICE (Stream FPS/Bitrate/Resolution/Audio, Wireless ADB, Activity Log)
class DrawerPanel {
public:
    DrawerPanel();

    void setProfilesDir(const std::string& dir, const std::string& activePath);
    void refreshProfileList();

    void setActiveTab(int tab);
    int activeTab() const { return activeTab_; }
    void setLatencyDiagnostics(const TelemetrySummary& vidTel, const ProbeResult& probe, const std::string& presentMode) {
        vidTelemetry_ = vidTel;
        probeResult_ = probe;
        presentModeLabel_ = presentMode;
    }

    bool isTypingText() const { return editingNewProfileName_ || editingNodeLabel_ || editingWireless_; }
    bool isWaitingSpecialRebind() const { return rebindingToggleAim_ || rebindingHoldUnlock_; }
    bool isRebindingToggleAim() const { return rebindingToggleAim_; }
    bool isRebindingHoldUnlock() const { return rebindingHoldUnlock_; }
    bool isDraggingSlider() const { return activeSliderId_ != 0; }

    void clearFocus() {
        editingNewProfileName_ = false;
        editingNodeLabel_ = false;
        editingWireless_ = false;
        rebindingToggleAim_ = false;
        rebindingHoldUnlock_ = false;
    }

    void paint(Gdiplus::Graphics& g, const RECT& bounds,
               const Profile& profile, const AppSettings& settings,
               const std::string& activeProfilePath,
               int selectedBtnIndex, bool waitingNodeRebind,
               bool connected, const std::string& deviceLabel,
               const std::string& foregroundPkg,
               const std::vector<std::string>& logs,
               int hoverX, int hoverY);

    bool containsPoint(int x, int y) const;

    bool onMouseDown(int x, int y, Profile& profile, AppSettings& settings,
                     int& selectedBtnIndex, DrawerActionResult& out);
    bool onMouseMove(int x, int y, Profile& profile, int selectedBtnIndex, DrawerActionResult& out);
    bool onMouseUp();
    bool onMouseWheel(int x, int y, int delta);
    bool onKeyDown(int vk, Profile& profile, AppSettings& settings,
                   int selectedBtnIndex, DrawerActionResult& out);
    bool onMouseBind(int mouseVk, Profile& profile, DrawerActionResult& out);
    bool onChar(char ch, Profile& profile, AppSettings& settings,
                int selectedBtnIndex, DrawerActionResult& out);

private:
    struct Hotspot {
        RECT rc;
        int id;
        int extra;
    };

    struct SliderHit {
        RECT trackRc;
        RECT hitRc;
        int id;
        float minVal;
        float maxVal;
    };

    struct ProfileEntry {
        std::string path;
        std::string fileName;
        std::string displayName;
    };

    void drawSliderRow(Gdiplus::Graphics& g, int x, int y, int w,
                       const char* label, float val, float minV, float maxV,
                       const char* valText, uint32_t accent, int sliderId,
                       int hoverX, int hoverY);
    void drawToggleRow(Gdiplus::Graphics& g, int x, int y, int w,
                       const char* label, bool on, uint32_t accent, int actionId,
                       int hoverX, int hoverY);
    void applySliderValue(int sliderId, float normT, Profile& p, int selectedBtnIndex);

    int paintTabKeys(Gdiplus::Graphics& g, int x, int y, int w,
                     const Profile& p, int selectedBtnIndex, bool waitingRebind,
                     int hoverX, int hoverY);
    int paintTabSens(Gdiplus::Graphics& g, int x, int y, int w,
                     const Profile& p, int hoverX, int hoverY);
    int paintTabProfiles(Gdiplus::Graphics& g, int x, int y, int w,
                         const Profile& p, const std::string& activePath,
                         const std::string& foregroundPkg, int hoverX, int hoverY);
    int paintTabDevice(Gdiplus::Graphics& g, int x, int y, int w,
                       const AppSettings& s, bool connected, const std::string& deviceLabel,
                       const std::vector<std::string>& logs, int hoverX, int hoverY);

    RECT bounds_;
    RECT bodyClip_;
    int activeTab_ = 0;
    int scrollY_[4];
    int maxScrollY_ = 0;

    std::vector<Hotspot> hotspots_;
    std::vector<SliderHit> sliders_;

    int activeSliderId_ = 0;
    RECT activeSliderTrack_;

    std::string profilesDir_;
    std::string activeProfilePath_;
    std::vector<ProfileEntry> profileEntries_;
    std::string newProfileName_ = "my_custom";

    bool editingNewProfileName_ = false;
    bool editingNodeLabel_ = false;
    bool editingWireless_ = false;
    bool rebindingToggleAim_ = false;
    bool rebindingHoldUnlock_ = false;

    TelemetrySummary vidTelemetry_;
    ProbeResult probeResult_;
    std::string presentModeLabel_;
};
