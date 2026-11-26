#pragma once

#include "bridge/ConnectionState.hpp"
#include "bridge/QrCodeGen.hpp"
#include "bridge/ScrcpyBridge.hpp"
#include "ui/GdiPlusDraw.hpp"

#include <string>
#include <vector>

enum OnboardingStep {
    OB_STEP_TOOLS = 0,
    OB_STEP_PREPARE = 1,
    OB_STEP_CONNECT = 2,
    OB_STEP_PROFILE = 3,
    OB_STEP_TOUR = 4,
    OB_STEP_DONE = 5
};

enum OnboardingProfileChoice {
    OB_PROF_DESKTOP = 0,
    OB_PROF_CONTROLLER = 1,
    OB_PROF_GAMING = 2
};

enum OnboardingAction {
    OB_ACT_NONE = 0,
    OB_ACT_REPAIR_TOOLS,
    OB_ACT_RECHECK_TOOLS,
    OB_ACT_CONNECT_DEVICE,
    OB_ACT_CONNECT_WIFI,
    OB_ACT_TOGGLE_QR,
    OB_ACT_APPLY_PROFILE_CHOICE,
    OB_ACT_FINISH,
    OB_ACT_SKIP
};

struct OnboardingClickResult {
    OnboardingAction action;
    int index;
    OnboardingProfileChoice profileChoice;
    bool redraw;

    OnboardingClickResult()
        : action(OB_ACT_NONE), index(-1), profileChoice(OB_PROF_DESKTOP), redraw(false) {}
};

// F1: Full-viewport 5-step first-run onboarding wizard drawn inside ShellWindow's viewport area.
class OnboardingView {
public:
    OnboardingView();
    ~OnboardingView();

    static OnboardingStep computeRecommendedStep(OnboardingStep current,
                                                 bool toolsReady,
                                                 bool connected,
                                                 bool hasAuthorizedDevice,
                                                 bool hasUnauthorizedDevice);

    void setStep(OnboardingStep step);
    OnboardingStep step() const { return step_; }

    void setWirelessTab(bool wireless);
    bool isWirelessTab() const { return wirelessTab_; }
    bool isEditingWifi() const { return editingWifi_; }
    void clearFocus() { editingWifi_ = false; }

    OnboardingProfileChoice selectedProfileChoice() const { return profileChoice_; }
    void setSelectedProfileChoice(OnboardingProfileChoice c) { profileChoice_ = c; }

    void paint(Gdiplus::Graphics& g, const RECT& viewportRc,
               bool toolsReady, bool connected, bool connecting, bool repairingTools,
               const std::string& toolsErr, const std::string& statusMsg,
               const std::vector<AdbDeviceInfo>& devices,
               const std::string& wirelessAddr,
               bool showingQrPair, const QrMatrix* qrMatrix, const std::string& qrStatus,
               int hoverX, int hoverY);

    bool onMouseDown(int x, int y, OnboardingClickResult& out);
    bool onKeyDown(int vk, std::string& wirelessAddr, OnboardingClickResult& out);
    bool onChar(char ch, std::string& wirelessAddr);
    bool hoverItemAt(int x, int y, int& outId, RECT& outRc) const;
    bool rectForStepButton(int buttonId, RECT& outRc) const;

private:
    enum InternalButtonId {
        BTN_SKIP = 1,
        BTN_BACK = 2,
        BTN_PRIMARY = 3,
        BTN_SECONDARY = 4,
        BTN_TAB_USB = 5,
        BTN_TAB_WIFI = 6,
        BTN_WIFI_INPUT = 7,
        BTN_WIFI_CONNECT = 8,
        BTN_WIFI_QR = 9,
        BTN_PROF_DESKTOP = 10,
        BTN_PROF_CONTROLLER = 11,
        BTN_PROF_GAMING = 12,
        BTN_STEP_BASE = 20,
        BTN_DEV_BASE = 100
    };

    struct Hotspot {
        RECT rc;
        int id;
        int extra;
    };

    OnboardingStep step_;
    OnboardingProfileChoice profileChoice_;
    bool wirelessTab_;
    bool editingWifi_;
    std::vector<Hotspot> hotspots_;
    Gdiplus::Bitmap* cachedQrBmp_;
    int cachedQrSize_;
    int cachedQrCellPx_;
    uint32_t cachedQrHash_;
};
