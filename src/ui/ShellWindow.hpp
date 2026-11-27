#pragma once

#include "audio/AudioPlayer.hpp"
#include "bridge/ConnectionState.hpp"
#include "bridge/ScrcpyBridge.hpp"
#include "config/AppSettings.hpp"
#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "engine/InputHookThread.hpp"
#include "ui/Animator.hpp"
#include "ui/DrawerPanel.hpp"
#include "ui/HudOverlay.hpp"
#include "ui/OnboardingView.hpp"
#include "ui/OverlayPopups.hpp"
#include "ui/SideRail.hpp"
#include "ui/Theme.hpp"
#include "ui/TopBar.hpp"
#include "ui/ViewportHost.hpp"
#include "video/H264Decoder.hpp"
#include "video/LatencyTelemetry.hpp"

#include <map>
#include <string>
#include <vector>

// Master frameless production Emulator Shell window (BlueStacks / LDPlayer style):
//   - TopBar (40px) + Center ViewportHost (embedded borderless scrcpy 5.0 OR Device Hub)
//   - Docked Outside-Viewport DrawerPanel (356px) + Right SideRail (54px)
//   - Owned HudOverlay pinned strictly over the active phone video rect
//   - Raw Mouse Input (WM_INPUT) + non-blocking WH_MOUSE_LL / WH_KEYBOARD_LL hooks
//   - Fullscreen gaming (F11), Android nav controls, screenshots, Wireless ADB, atomic auto-save
class ShellWindow : public IHudOverlayHost, public IOverlayPopupHost {
public:
    ShellWindow(ScrcpyBridge* bridge, InputEngine* engine,
                const std::string& exeDir, const std::string& profilesDir,
                const std::string& profilePath, const std::string& settingsPath,
                bool startDemo, bool noMirror);
    ~ShellWindow();

    void setLaunchOptions(bool consoleEnabled, bool probeOnConnect, int probeSamples,
                          bool startFullscreen = false,
                          bool forceAudio = false,
                          const std::string& forceAudioCodec = "",
                          int forceAudioBufferMs = 0) {
        consoleEnabled_ = consoleEnabled;
        autoProbeOnConnect_ = probeOnConnect;
        autoProbeSamples_ = probeSamples;
        autoFullscreen_ = startFullscreen;
        forceAudioOn_ = forceAudio;
        forceAudioCodec_ = forceAudioCodec;
        forceAudioBufferMs_ = forceAudioBufferMs;
        if (autoProbeOnConnect_) showOnboarding_ = false;
    }

    bool create(HINSTANCE hInst);
    int runMessageLoop();

    // IHudOverlayHost implementation
    Profile& hudProfile() override { return profile_; }
    EngineTelemetry hudTelemetry() const override { return engine_->getTelemetry(); }
    bool hudIsEditMode() const override { return editMode_; }
    bool hudIsDemoMode() const override { return demoMode_; }
    bool hudIsAimMode() const override { return aimMode_; }
    bool hudIsFullscreen() const override { return fullscreen_; }
    bool hudIsSpecialDrawerRebind() const override { return drawerOpen_ && drawerPanel_.isWaitingSpecialRebind(); }
    std::string hudSpecialRebindLabel() const override;
    bool hudIsSpecialAimRebind() const override { return drawerOpen_ && drawerPanel_.isRebindingToggleAim(); }

    void hudOnSelectionChanged(int newSelectedIdx) override;
    void hudOnProfileChanged(bool commitSaveNow) override;
    void hudOnRequestSave() override;
    void hudOnRequestCloseEdit() override;
    void hudOnMouseBindToDrawer(int mouseVk) override;
    void hudOnToast(const std::string& msg, DWORD durationMs) override;

    // IOverlayPopupHost implementation (X5 Command Palette, X6 Cheat Sheet, X7 Profile Dropdown)
    void onPopupPaint(Gdiplus::Graphics& g, int w, int h) override;
    void onPopupClick(int x, int y) override;
    void onPopupHover(int x, int y) override;
    void onPopupWheel(int delta) override;

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI startupToolsThreadProc(LPVOID param);
    static DWORD WINAPI refreshDevicesThreadProc(LPVOID param);
    static DWORD WINAPI deviceMonitorThreadProc(LPVOID param);
    static DWORD WINAPI repairToolsThreadProc(LPVOID param);
    static DWORD WINAPI connectThreadProc(LPVOID param);
    static DWORD WINAPI disconnectThreadProc(LPVOID param);
    static DWORD WINAPI legacyFallbackThreadProc(LPVOID param);
    static DWORD WINAPI wirelessThreadProc(LPVOID param);
    static DWORD WINAPI appWatcherThreadProc(LPVOID param);
    static DWORD WINAPI dropWorkerThreadProc(LPVOID param);
    static DWORD WINAPI qrPairThreadProc(LPVOID param);
    static DWORD WINAPI profileSaveWorkerProc(LPVOID param);
    static DWORD WINAPI settingsSaveWorkerProc(LPVOID param);
    static DWORD WINAPI screenshotThreadProc(LPVOID param);
    static DWORD WINAPI probeThreadProc(LPVOID param);
    static void onConnectStep(void* ctx, int step);
    static void onDeviceClipboard(void* ctx, const std::string& utf8Text);
    static void onPadNav(void* ctx, uint8_t navMask);
    static void onVideoPacket(void* ctx, const uint8_t* data, size_t size,
                              uint64_t ptsUs, bool isConfig, bool isKeyFrame,
                              bool present, int64_t qpcPktRecv);
    static void onAudioPacket(void* ctx, AudioCodecId codec,
                              const uint8_t* data, size_t size,
                              uint64_t ptsUs, bool isConfig);
    static void onDecodedGpuFrame(void* ctx, ID3D11Texture2D* nv12Tex, uint32_t subresourceIdx,
                                  uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                  const FrameTiming& timing);
    static void onDecodedCpuView(void* ctx, const uint8_t* yPlane, const uint8_t* uvPlane,
                                 uint32_t stride, uint32_t cropX, uint32_t cropY,
                                 uint32_t outW, uint32_t outH, const FrameTiming& timing);
    static void onHudBitmapReady(void* ctx, const void* bgraPremulPixels, int w, int h);

    bool mapShellClientToVideoLocal(int mx, int my, int& outVx, int& outVy, bool requireInside) const;
    bool mapShellClientToVideoNorm(int mx, int my, float& outNx, float& outNy, bool requireInside) const;

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT dispatchMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void handleLeftButtonDown(HWND hwnd, int mx, int my);
    void startAsyncLegacyFallback(const std::string& reason);
    void applyTuningToEngine();
    void syncInputHookSnapshot();

    void layoutChildren();
    void syncHudAndVideo();
    void updateClipCursor();
    void saveWindowPlacement();
    void scheduleSettingsSave();
    void saveSettingsNow();

    void startAsyncConnect(const std::string& serial);
    void startAsyncWirelessConnect(const std::string& ipPort);
    void startAsyncRepairTools();
    void startAsyncDropTransfer(const std::vector<std::string>& files);
    void startQrWirelessPairing();
    void stopQrWirelessPairing();
    void startRoundTripProbe(int numTrials = 60);
    void refreshAdbDevices();
    void disconnectDevice(bool returnToHub, bool userInitiated = true);
    void takeScreenshot();
    void applyOnboardingProfileChoice(OnboardingProfileChoice choice);
    void finishOnboarding();
    void buildHubViewState(HubViewState& out) const;

    void handleForegroundAppChanged(const std::string& pkg);
    bool switchProfilePath(const std::string& targetPath, bool showAutoToast);
    void recheckAutoSwitchForCurrentApp();
    void recordRecentProfile(const std::string& path);
    void rebuildProfileMenuItems();
    std::string ensureDesktopProfileExists();
    bool handleDesktopKeyEvent(int vk, bool down);
    bool handleDesktopChar(wchar_t ch);
    void sendPhoneText(const std::string& utf8);
    std::string readWindowsClipboardUtf8() const;
    void writeWindowsClipboardUtf8(const std::string& utf8Text, bool fromPhone = true);

    void setAimMode(bool enabled);
    void setDrawerState(bool open, int tab);
    void setEditMode(bool enabled);
    void toggleFullscreen();
    void toggleAlwaysOnTop();
    void toggleFireMode();
    void toggleHud();
    void triggerQuickReset();
    void showToast(const std::string& msg, DWORD durationMs = 2200, int toastKind = TOAST_INFO);
    void showSnackbar(const std::string& msg, const std::string& actionLabel, DWORD durationMs = 6000, int toastKind = TOAST_INFO);
    bool triggerShellUndo();
    RECT shellToastRect() const;
    void syncAnimTimer();
    void updateTooltipAtPoint(int mx, int my);
    void initCommandPalette();
    void openPopup(int mode);
    void closePopup();
    void executeCommandById(const std::string& cmdId);
    void handlePopupKeyDown(int vk);
    void handlePopupChar(wchar_t ch);

    void applyProfileToEngine(bool autoSaveNow = true);
    bool saveCurrentProfile(bool silent = false);
    void flushOnExit();
    void handleDrawerResult(const DrawerActionResult& res);
    void handleSideRailAction(SideRailAction act);
    void handleTopBarAction(TopBarAction act);

    bool isAppForeground() const;
    bool isPointInVideoScreen(POINT screenPt, float& outNormX, float& outNormY) const;
    bool ensurePaintCache(HDC refDc, int w, int h);
    void onPaint(HDC hdc, const RECT& dirtyRc);
    void invalidateShell();

    ScrcpyBridge* bridge_;
    InputEngine* engine_;
    std::string exeDir_;
    std::string profilesDir_;
    std::string profilePath_;
    std::string settingsPath_;
    std::string mirrorTitle_;
    bool noMirror_ = false;

    HINSTANCE hInst_ = NULL;
    HWND hwnd_ = NULL;
    InputHookThread hookThread_;
    LatencyTelemetry telemetry_;
    ProbeResult probeResult_;
    PhoneThermalInfo thermalInfo_;
    std::vector<VideoEncoderInfo> deviceEncoders_;
    std::string activeEncoderName_;
    std::string pendingDiscoveredSerial_;
    std::string pendingDiscoveredEncoder_;

    ToolPaths tools_;
    bool toolsReady_ = false;
    bool autoConnectPending_ = false;
    std::vector<AdbDeviceInfo> adbDevices_;
    std::string hubStatusMsg_;
    bool editingHubWireless_ = false;

    // Async connection, foreground-app watcher, clipboard sync, probe & drag-drop state
    struct AsyncContext {
        CRITICAL_SECTION lock;
        HWND hwnd = NULL;
        volatile LONG alive = 1;
        volatile LONG refCount = 1;
        AsyncContext() { InitializeCriticalSection(&lock); }
        ~AsyncContext() { DeleteCriticalSection(&lock); }
        void addRef() { InterlockedIncrement(&refCount); }
        void release() { if (InterlockedDecrement(&refCount) == 0) delete this; }
        void postMsg(UINT msg, WPARAM wp = 0, LPARAM lp = 0) {
            EnterCriticalSection(&lock);
            if (alive && hwnd && IsWindow(hwnd)) PostMessageW(hwnd, msg, wp, lp);
            LeaveCriticalSection(&lock);
        }
    };
    struct QrPairJob {
        AsyncContext* ctx = nullptr;
        ScrcpyBridge* bridge = nullptr;
        ToolPaths tools;
        std::string serviceName;
        std::string password;
        std::string* statusTarget = nullptr;
        std::string* autoConnectTarget = nullptr;
        CRITICAL_SECTION* auxLock = nullptr;
        volatile LONG cancelled = 0;
    };
    struct SimpleAsyncJob {
        ShellWindow* shell;
        AsyncContext* ctx;
        std::string arg;
        int intArg;
    };

    mutable CRITICAL_SECTION auxLock_;
    AsyncContext* asyncCtx_ = nullptr;
    QrPairJob* activeQrJob_ = nullptr;
    HANDLE probeEvent_ = NULL;
    HANDLE deviceMonitorWakeEvent_ = NULL;
    volatile LONG connecting_ = 0;
    volatile LONG refreshingDevices_ = 0;
    volatile LONG repairingTools_ = 0;
    volatile LONG stopAuxThreads_ = 0;
    volatile LONG dropping_ = 0;
    volatile LONG probing_ = 0;
    volatile LONG stopProbe_ = 0;
    volatile LONG settingsSaveInFlight_ = 0;
    bool showingQrPair_ = false;
    std::string qrServiceName_;
    std::string qrPairPassword_;
    std::string qrPairStatus_;
    std::string qrAutoConnectAddr_;
    QrMatrix qrMatrix_;
    std::string pendingSerial_;
    std::string pendingWirelessIp_;
    std::string asyncResultMsg_;
    bool asyncResultOk_ = false;
    std::string pendingScreenshotToast_;
    std::vector<AdbDeviceInfo> pendingAdbDevices_;
    ToolPaths pendingTools_;
    std::string pendingToolsErr_;
    bool pendingToolsOk_ = false;
    bool pendingDevicesRefreshOk_ = false;

    std::string activeAppPackage_;
    std::string pendingForegroundPkg_;
    bool pendingSecureFlag_ = false;
    bool secureAppDetected_ = false;
    std::string lastGameProfilePath_;
    std::string manualOverridePkg_;
    std::map<std::string, std::string> sessionAppProfiles_; // E5: session app -> profile memory
    std::string pendingDeviceClip_;
    std::string lastSyncedClip_;
    std::string lastDeviceClip_;
    ClipboardSyncGuard clipGuard_;
    bool clipboardListenerActive_ = false;
    std::vector<std::string> pendingDropFiles_;
    std::string pendingDropToast_;
    std::string pendingFallbackReason_;
    bool rightClickBackSwallowUp_ = false;
    wchar_t highSurrogate_ = 0;

    Profile profile_;
    bool profileDirty_ = false;
    AppSettings settings_;

    // Modular outside-viewport UI components
    TopBar topBar_;
    SideRail sideRail_;
    DrawerPanel drawerPanel_;
    ViewportHost viewportHost_;
    OnboardingView onboardingView_;
    HudOverlay hudOverlay_;

    RECT topBarRc_;
    RECT railRc_;
    RECT drawerRc_;
    RECT viewportRc_;

    bool demoMode_ = false;
    bool drawerOpen_ = false;
    bool editMode_ = false;
    bool editTestLive_ = false;
    bool aimMode_ = false;
    bool fullscreen_ = false;
    RECT preFullscreenRect_;
    bool preFullscreenMax_ = false;

    bool holdUnlockedAim_ = false;
    bool cursorDirectDragging_ = false;
    bool middlePanDragging_ = false;
    int wheelDeltaAccum_ = 0;
    DWORD lastWheelTick_ = 0;
    float lastCursorNormX_ = 0.5f;
    float lastCursorNormY_ = 0.5f;
    POINT lastUnlockedCursorPt_;
    bool hasLastUnlockedCursorPt_ = false;

    int hoverX_ = -1;
    int hoverY_ = -1;
    bool hoveringChrome_ = false;
    int addNodeStagger_ = 0;
    uint32_t lastPhoneW_ = 1920;
    uint32_t lastPhoneH_ = 1080;
    DWORD lastTelemetryPaintTick_ = 0;
    DWORD lastTopBarPaintTick_ = 0;
    DWORD pendingVdResizeTick_ = 0;
    bool hasPendingVdResize_ = false;
    DWORD profileSaveDueTick_ = 0;
    bool hasPendingProfileSave_ = false;
    DWORD settingsSaveDueTick_ = 0;
    bool hasPendingSettingsSave_ = false;
    bool sawEmbeddedMirror_ = false;
    bool nativeVideoSession_ = false;
    bool d3dHudDragging_ = false;
    bool lastAppliedScreenOff_ = false;
    bool consoleEnabled_ = false;
    bool autoProbeOnConnect_ = false;
    bool autoFullscreen_ = false;
    bool forceAudioOn_ = false;
    std::string forceAudioCodec_;
    int forceAudioBufferMs_ = 0;
    int autoProbeSamples_ = 60;
    bool autoProbeArmed_ = false;
    DWORD autoProbeDueTick_ = 0;
    int probeRunCount_ = 0;
    bool debugOverlayVisible_ = false;
    uint32_t uiStallCount_ = 0;
    double maxUiMsgMs_ = 0.0;
    double lastPaintMs_ = 0.0;
    volatile LONG aimModeAtomic_ = 0;
    volatile LONG autoSwitchAtomic_ = 1;
    volatile LONG gameProfileAtomic_ = 1;
    volatile LONG deviceResetPending_ = 0;
    volatile LONG reinitDecoderFlag_ = 0;
    volatile LONG fallbackPending_ = 0;
    H264Decoder h264Decoder_;
    AudioPlayer audioPlayer_;

    struct HudVisualSig {
        bool valid = false;
        bool aimMode = false;
        bool aimFingerDown = false;
        bool fireHeld = false;
        bool scopeActive = false;
        bool holdViewActive = false;
        bool joyDown = false;
        bool showHud = false;
        bool fullscreen = false;
        int aimQx = -1;
        int aimQy = -1;
        uint32_t fsRate = 0;
        uint64_t activeBtnMask = 0;
    };
    HudVisualSig lastHudSig_;

    struct TopBarVisualSig {
        bool valid = false;
        bool connected = false;
        bool reconnecting = false;
        bool connLost = false;
        bool aimMode = false;
        bool keymapEnabled = false;
        int fireMode = 0;
        uint32_t touchRateHz = 0;
        int latP50Q10 = -1;
        int latP95Q10 = -1;
        uint64_t skipped = 0;
    };
    TopBarVisualSig lastTopBarSig_;

    bool cursorHidden_ = false;
    bool hadForeground_ = false;
    Theme::Metrics metrics_;
    bool dwmRoundedActive_ = false;
    bool inSizeMove_ = false;
    DWORD lastResizeTick_ = 0;
    int lastHoverRegion_ = 0;
    int lastHoverItemId_ = -1;
    RECT lastHoverRc_ = { 0, 0, 0, 0 };
    std::string shellToastMsg_;
    std::string shellToastActionLabel_;
    int shellToastKind_ = TOAST_INFO;
    RECT shellToastActionRc_ = {0, 0, 0, 0};
    DWORD shellToastExpireTick_ = 0;
    bool shellToastWasVisible_ = false;
    enum PendingUndoKind { UNDO_NONE = 0, UNDO_HUD_EDIT, UNDO_TRASH_PROFILE, UNDO_STAY_FOR_APP };
    PendingUndoKind pendingUndoKind_ = UNDO_NONE;
    std::string undoTrashPath_;
    std::string undoTrashOrigPath_;

    // U4 / U6 / X5 / X6 / X7 Floating Overlays & Motion System
    enum ActivePopupMode {
        POPUP_NONE = 0,
        POPUP_CMD_PALETTE,
        POPUP_CHEAT_SHEET,
        POPUP_PROFILE_DROPDOWN
    };
    Animator animator_;
    bool animTimerActive_ = false;
    LayeredTooltipWindow tooltipPopup_;
    LayeredModalPopup modalPopup_;
    ActivePopupMode activePopup_ = POPUP_NONE;
    CommandPaletteModel cmdPalette_;
    std::vector<CommandEntry> baseCommands_;
    std::string popupQuery_;
    std::vector<int> popupFiltered_;
    int popupSelectedRow_ = 0;
    int popupHoverRow_ = -1;
    std::vector<RECT> popupRowRects_;
    RECT popupManageProfilesRc_ = { 0, 0, 0, 0 };
    CheatSheetData cheatSheetData_;
    std::vector<ProfileMenuItem> profileMenuItems_;
    std::vector<std::string> recentProfilePaths_;
    std::string manualSwitchedAppPkg_;

    // F1 / F3 / X2 / F16 Connection Manager, Onboarding & Pending-Stream State
    bool showOnboarding_ = false;
    std::string lastConnectedSerial_;
    bool lastConnectedWireless_ = false;
    bool userDisconnected_ = false;
    bool reconnecting_ = false;
    bool reconnectCancelled_ = false;
    int reconnectAttempt_ = 0;
    DWORD reconnectNextTick_ = 0;
    int lastCountdownShown_ = -1;
    volatile LONG connectStep_ = 0;
    std::string lastConnectError_;
    bool hasPendingDeviceChange_ = false;
    DWORD deviceChangeDebounceTick_ = 0;
    AppSettings appliedStreamSettings_;
    bool hasAppliedStreamSettings_ = false;

    // Double-buffered shell GDI surface
    HDC cacheDC_ = NULL;
    HBITMAP cacheBmp_ = NULL;
    HGDIOBJ cacheOldBmp_ = NULL;
    int cacheW_ = 0;
    int cacheH_ = 0;
};
