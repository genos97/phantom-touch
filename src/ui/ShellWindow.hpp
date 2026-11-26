#pragma once

#include "bridge/ScrcpyBridge.hpp"
#include "config/AppSettings.hpp"
#include "config/Profile.hpp"
#include "engine/InputEngine.hpp"
#include "ui/DrawerPanel.hpp"
#include "ui/HudOverlay.hpp"
#include "ui/SideRail.hpp"
#include "ui/TopBar.hpp"
#include "ui/ViewportHost.hpp"
#include "video/H264Decoder.hpp"

#include <string>
#include <vector>

// Master frameless production Emulator Shell window (BlueStacks / LDPlayer style):
//   - TopBar (40px) + Center ViewportHost (embedded borderless scrcpy 5.0 OR Device Hub)
//   - Docked Outside-Viewport DrawerPanel (356px) + Right SideRail (54px)
//   - Owned HudOverlay pinned strictly over the active phone video rect
//   - Raw Mouse Input (WM_INPUT) + non-blocking WH_MOUSE_LL / WH_KEYBOARD_LL hooks
//   - Fullscreen gaming (F11), Android nav controls, screenshots, Wireless ADB, atomic auto-save
class ShellWindow : public IHudOverlayHost {
public:
    ShellWindow(ScrcpyBridge* bridge, InputEngine* engine,
                const std::string& exeDir, const std::string& profilesDir,
                const std::string& profilePath, const std::string& settingsPath,
                bool startDemo, bool noMirror);
    ~ShellWindow();

    bool create(HINSTANCE hInst);
    int runMessageLoop();

    LRESULT handleLowLevelMouse(int nCode, WPARAM wParam, LPARAM lParam);
    LRESULT handleLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam);

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

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static DWORD WINAPI connectThreadProc(LPVOID param);
    static DWORD WINAPI wirelessThreadProc(LPVOID param);
    static DWORD WINAPI appWatcherThreadProc(LPVOID param);
    static DWORD WINAPI dropWorkerThreadProc(LPVOID param);
    static DWORD WINAPI qrPairThreadProc(LPVOID param);
    static void onDeviceClipboard(void* ctx, const std::string& utf8Text);
    static void onVideoPacket(void* ctx, const uint8_t* data, size_t size,
                              uint64_t ptsUs, bool isConfig, bool isKeyFrame);
    static void onDecodedFrame(void* ctx, const DecodedNV12Frame& frame);
    static void onHudBitmapReady(void* ctx, const void* bgraPremulPixels, int w, int h);

    bool mapShellClientToVideoLocal(int mx, int my, int& outVx, int& outVy, bool requireInside) const;
    bool mapShellClientToVideoNorm(int mx, int my, float& outNx, float& outNy, bool requireInside) const;

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void registerRawInput();
    void installHooks();
    void removeHooks();

    void layoutChildren();
    void syncHudAndVideo();
    void updateClipCursor();
    void saveWindowPlacement();
    void saveSettings();

    void startAsyncConnect(const std::string& serial);
    void startAsyncWirelessConnect(const std::string& ipPort);
    void startAsyncDropTransfer(const std::vector<std::string>& files);
    void startQrWirelessPairing();
    void stopQrWirelessPairing();
    void refreshAdbDevices();
    void disconnectDevice(bool returnToHub);
    void takeScreenshot();

    void handleForegroundAppChanged(const std::string& pkg);
    bool switchProfilePath(const std::string& targetPath, bool showAutoToast);
    std::string ensureDesktopProfileExists();
    bool handleDesktopKeyEvent(int vk, bool down);
    bool handleDesktopChar(wchar_t ch);
    std::string readWindowsClipboardUtf8() const;
    void writeWindowsClipboardUtf8(const std::string& utf8Text);

    void setAimMode(bool enabled);
    void setDrawerState(bool open, int tab);
    void setEditMode(bool enabled);
    void toggleFullscreen();
    void toggleAlwaysOnTop();
    void toggleFireMode();
    void toggleHud();
    void triggerQuickReset();

    void applyProfileToEngine(bool autoSaveNow = true);
    bool saveCurrentProfile(bool silent = false);
    void flushOnExit();
    void handleDrawerResult(const DrawerActionResult& res);
    void handleSideRailAction(SideRailAction act);
    void handleTopBarAction(TopBarAction act);

    bool isAppForeground() const;
    bool isPointInVideoScreen(POINT screenPt, float& outNormX, float& outNormY) const;
    void onPaint(HDC hdc);
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
    HHOOK mouseHook_ = NULL;
    HHOOK keyHook_ = NULL;

    ToolPaths tools_;
    bool toolsReady_ = false;
    std::vector<AdbDeviceInfo> adbDevices_;
    std::string hubStatusMsg_;
    bool editingHubWireless_ = false;

    // Async connection, foreground-app watcher, clipboard sync & drag-drop state
    mutable CRITICAL_SECTION auxLock_;
    HANDLE connectThread_ = NULL;
    HANDLE appWatcherThread_ = NULL;
    HANDLE dropThread_ = NULL;
    HANDLE qrPairThread_ = NULL;
    volatile LONG connecting_ = 0;
    volatile LONG stopAuxThreads_ = 0;
    volatile LONG dropping_ = 0;
    volatile LONG stopQrPair_ = 0;
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

    std::string activeAppPackage_;
    std::string pendingForegroundPkg_;
    std::string lastGameProfilePath_;
    std::string pendingDeviceClip_;
    std::string lastSyncedClip_;
    std::vector<std::string> pendingDropFiles_;
    std::string pendingDropToast_;
    bool rightClickBackSwallowUp_ = false;

    Profile profile_;
    bool profileDirty_ = false;
    AppSettings settings_;

    // Modular outside-viewport UI components
    TopBar topBar_;
    SideRail sideRail_;
    DrawerPanel drawerPanel_;
    ViewportHost viewportHost_;
    HudOverlay hudOverlay_;

    RECT topBarRc_;
    RECT railRc_;
    RECT drawerRc_;
    RECT viewportRc_;

    bool demoMode_ = false;
    bool drawerOpen_ = false;
    bool editMode_ = false;
    bool aimMode_ = false;
    bool fullscreen_ = false;
    RECT preFullscreenRect_;
    bool preFullscreenMax_ = false;

    bool holdUnlockedAim_ = false;
    bool toggleAimKeyDown_ = false;
    bool hookKeyDown_[256];
    bool hadForeground_ = false;
    bool cursorHidden_ = false;
    bool cursorDirectDragging_ = false;
    POINT lastUnlockedCursorPt_;
    bool hasLastUnlockedCursorPt_ = false;

    int hoverX_ = -1;
    int hoverY_ = -1;
    int addNodeStagger_ = 0;
    uint32_t lastPhoneW_ = 1920;
    uint32_t lastPhoneH_ = 1080;
    DWORD lastTelemetryPaintTick_ = 0;
    bool sawEmbeddedMirror_ = false;
    bool nativeVideoSession_ = false;
    bool d3dHudDragging_ = false;
    bool lastAppliedScreenOff_ = false;
    volatile LONG videoFramePending_ = 0;
    H264Decoder h264Decoder_;

    // Double-buffered shell GDI surface
    HDC cacheDC_ = NULL;
    HBITMAP cacheBmp_ = NULL;
    HGDIOBJ cacheOldBmp_ = NULL;
    int cacheW_ = 0;
    int cacheH_ = 0;
};
