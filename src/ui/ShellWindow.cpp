#include "ui/ShellWindow.hpp"
#include "config/KeyCodes.hpp"
#include "config/ProfileStore.hpp"
#include "ui/Theme.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

static ShellWindow* g_shellInstance = NULL;
static const UINT_PTR TIMER_TICK = 1;

// Custom WM_APP messages for safe cross-thread / hook-to-UI transitions
static const UINT MSG_TOGGLE_AIM         = WM_APP + 1;
static const UINT MSG_UNLOCK_AIM         = WM_APP + 2;
static const UINT MSG_TOGGLE_STUDIO      = WM_APP + 3;
static const UINT MSG_CYCLE_FIRE         = WM_APP + 4;
static const UINT MSG_TOGGLE_HUD         = WM_APP + 5;
static const UINT MSG_RESET_TOUCH        = WM_APP + 6;
static const UINT MSG_LOCK_AIM           = WM_APP + 7;
static const UINT MSG_TOGGLE_FULLSCREEN  = WM_APP + 8;
static const UINT MSG_CONNECT_DONE       = WM_APP + 9;
static const UINT MSG_SENS_X_DOWN        = WM_APP + 10;
static const UINT MSG_SENS_X_UP          = WM_APP + 11;
static const UINT MSG_SENS_Y_DOWN        = WM_APP + 12;
static const UINT MSG_SENS_Y_UP          = WM_APP + 13;
static const UINT MSG_WIRELESS_DONE      = WM_APP + 14;

LRESULT CALLBACK shellMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_shellInstance) return g_shellInstance->handleLowLevelMouse(nCode, wParam, lParam);
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK shellKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_shellInstance) return g_shellInstance->handleLowLevelKeyboard(nCode, wParam, lParam);
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

float clamp01(float v) {
    if (v < 0.01f) return 0.01f;
    if (v > 0.99f) return 0.99f;
    return v;
}

float clampRange(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

bool ptIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

} // namespace

ShellWindow::ShellWindow(ScrcpyBridge* bridge, InputEngine* engine,
                         const std::string& exeDir, const std::string& profilesDir,
                         const std::string& profilePath, const std::string& settingsPath,
                         bool startDemo, bool noMirror)
    : bridge_(bridge),
      engine_(engine),
      exeDir_(exeDir),
      profilesDir_(profilesDir),
      profilePath_(profilePath),
      settingsPath_(settingsPath),
      noMirror_(noMirror),
      hudOverlay_(this),
      demoMode_(startDemo) {
    ZeroMemory(&topBarRc_, sizeof(topBarRc_));
    ZeroMemory(&railRc_, sizeof(railRc_));
    ZeroMemory(&drawerRc_, sizeof(drawerRc_));
    ZeroMemory(&viewportRc_, sizeof(viewportRc_));
    ZeroMemory(&preFullscreenRect_, sizeof(preFullscreenRect_));
    ZeroMemory(&lastUnlockedCursorPt_, sizeof(lastUnlockedCursorPt_));
    std::memset(hookKeyDown_, 0, sizeof(hookKeyDown_));

    char tbuf[64];
    std::snprintf(tbuf, sizeof(tbuf), "PhantomTouch-Video-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    mirrorTitle_ = tbuf;

    profile_ = engine_->getProfileCopy();
    AppSettingsStore::load(settingsPath_, settings_);
    drawerPanel_.setProfilesDir(profilesDir_, profilePath_);
}

ShellWindow::~ShellWindow() {
    flushOnExit();
    setAimMode(false);
    removeHooks();
    if (connectThread_) {
        WaitForSingleObject(connectThread_, 1500);
        CloseHandle(connectThread_);
        connectThread_ = NULL;
    }
    viewportHost_.detachMirror();
    hudOverlay_.destroy();
    if (cacheDC_) {
        if (cacheOldBmp_) SelectObject(cacheDC_, cacheOldBmp_);
        if (cacheBmp_) DeleteObject(cacheBmp_);
        DeleteDC(cacheDC_);
        cacheDC_ = NULL;
        cacheBmp_ = NULL;
        cacheOldBmp_ = NULL;
    }
    if (g_shellInstance == this) g_shellInstance = NULL;
}

bool ShellWindow::create(HINSTANCE hInst) {
    hInst_ = hInst;
    g_shellInstance = this;

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = ShellWindow::wndProc;
    wc.hInstance = hInst_;
    wc.hIcon = LoadIconA(hInst_, MAKEINTRESOURCEA(101));
    wc.hIconSm = LoadIconA(hInst_, MAKEINTRESOURCEA(101));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "PhantomTouchShellClass";

    if (!RegisterClassExA(&wc)) return false;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int w = settings_.winW;
    int h = settings_.winH;
    if (w < Theme::MIN_WIN_W) w = Theme::MIN_WIN_W;
    if (h < Theme::MIN_WIN_H) h = Theme::MIN_WIN_H;
    if (w > screenW) w = screenW - 40;
    if (h > screenH) h = screenH - 60;

    int x = settings_.winX;
    int y = settings_.winY;
    if (x < -100 || y < -100 || x > screenW - 120 || y > screenH - 120) {
        x = (screenW - w) / 2;
        y = (screenH - h) / 2;
    }

    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_APPWINDOW | (settings_.alwaysOnTop ? WS_EX_TOPMOST : 0);

    hwnd_ = CreateWindowExA(exStyle, wc.lpszClassName, "Phantom Touch",
                            style, x, y, w, h, NULL, NULL, hInst_, this);
    if (!hwnd_) return false;

    GdiPlusDraw::enableDarkDwmFrame(hwnd_);
    viewportHost_.create(hwnd_, hInst_);
    hudOverlay_.create(hwnd_, hInst_);

    registerRawInput();
    installHooks();
    SetTimer(hwnd_, TIMER_TICK, 16, NULL);

    // Locate official scrcpy + adb tools
    std::string toolErr;
    toolsReady_ = bridge_->locateTools(tools_, toolErr);
    if (!toolsReady_) {
        hubStatusMsg_ = toolErr;
        bridge_->appendLog("[Tools] " + toolErr);
    } else {
        bridge_->appendLog("[Tools] scrcpy " + tools_.scrcpyVersion + " ready");
    }

    if (settings_.maximized) {
        ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    } else {
        ShowWindow(hwnd_, SW_SHOW);
    }
    UpdateWindow(hwnd_);

    if (demoMode_) {
        drawerOpen_ = true;
        drawerPanel_.setActiveTab(0);
        setEditMode(true);
        layoutChildren();
        hudOverlay_.showToast("Offline Keymap Studio ready! Edit keys on canvas or in the Right Drawer.", 4000);
    } else {
        layoutChildren();
        refreshAdbDevices();
        // Auto-connect immediately if a ready phone is already plugged in!
        for (size_t i = 0; i < adbDevices_.size(); ++i) {
            if (adbDevices_[i].state == "device") {
                startAsyncConnect(adbDevices_[i].serial);
                break;
            }
        }
    }
    return true;
}

int ShellWindow::runMessageLoop() {
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void ShellWindow::registerRawInput() {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void ShellWindow::installHooks() {
    mouseHook_ = SetWindowsHookExA(WH_MOUSE_LL, shellMouseHookProc, hInst_, 0);
    keyHook_   = SetWindowsHookExA(WH_KEYBOARD_LL, shellKeyHookProc, hInst_, 0);
}

void ShellWindow::removeHooks() {
    if (mouseHook_) { UnhookWindowsHookEx(mouseHook_); mouseHook_ = NULL; }
    if (keyHook_)   { UnhookWindowsHookEx(keyHook_);   keyHook_ = NULL; }
}

void ShellWindow::invalidateShell() {
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void ShellWindow::saveWindowPlacement() {
    if (!hwnd_ || fullscreen_) return;
    WINDOWPLACEMENT wp;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (GetWindowPlacement(hwnd_, &wp)) {
        settings_.maximized = (wp.showCmd == SW_SHOWMAXIMIZED);
        settings_.winX = wp.rcNormalPosition.left;
        settings_.winY = wp.rcNormalPosition.top;
        settings_.winW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        settings_.winH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    }
}

void ShellWindow::saveSettings() {
    saveWindowPlacement();
    AppSettingsStore::save(settingsPath_, settings_);
}

void ShellWindow::applyProfileToEngine(bool autoSaveNow) {
    engine_->setProfile(profile_);
    hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
    profileDirty_ = true;
    if (autoSaveNow) {
        saveCurrentProfile(true);
    }
    hudOverlay_.invalidate();
    invalidateShell();
}

bool ShellWindow::saveCurrentProfile(bool silent) {
    if (profilePath_.empty()) return false;
    if (ProfileStore::save(profilePath_, profile_)) {
        ProfileStore::saveLastActive(profilesDir_, profilePath_);
        profileDirty_ = false;
        if (!silent) {
            drawerPanel_.refreshProfileList();
            hudOverlay_.showToast("Saved config -> " + profile_.name, 2000);
        }
        return true;
    }
    if (!silent) {
        hudOverlay_.showToast("ERROR: Could not write " + profilePath_, 3000);
    }
    return false;
}

void ShellWindow::flushOnExit() {
    if (!profilePath_.empty()) {
        if (profileDirty_) saveCurrentProfile(true);
        else ProfileStore::saveLastActive(profilesDir_, profilePath_);
    }
    saveSettings();
}

void ShellWindow::layoutChildren() {
    if (!hwnd_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (fullscreen_) {
        topBarRc_   = { 0, 0, 0, 0 };
        railRc_     = { 0, 0, 0, 0 };
        drawerRc_   = { 0, 0, 0, 0 };
        viewportRc_ = { 0, 0, w, h };
    } else {
        topBarRc_ = { 0, 0, w, Theme::TOPBAR_H };
        railRc_   = { w - Theme::RAIL_W, Theme::TOPBAR_H, w, h };
        int rightEdge = railRc_.left;
        if (drawerOpen_) {
            int dw = Theme::DRAWER_W;
            if (rightEdge - dw < 360) dw = rightEdge - 360;
            if (dw < 260) dw = 260;
            drawerRc_ = { rightEdge - dw, Theme::TOPBAR_H, rightEdge, h };
            rightEdge = drawerRc_.left;
        } else {
            drawerRc_ = { 0, 0, 0, 0 };
        }
        viewportRc_ = { 0, Theme::TOPBAR_H, rightEdge, h };
    }

    viewportHost_.setBounds(viewportRc_);
    syncHudAndVideo();
    invalidateShell();
}

void ShellWindow::syncHudAndVideo() {
    if (!hwnd_) return;
    bool minimized = IsIconic(hwnd_) != FALSE;
    bool connected = bridge_->isConnected();

    if (connected) {
        uint32_t pw = 1920, ph = 1080;
        bridge_->getScreenSize(pw, ph);
        if (pw != lastPhoneW_ || ph != lastPhoneH_) {
            lastPhoneW_ = pw;
            lastPhoneH_ = ph;
            viewportHost_.updateVideoLayout(pw, ph);
            invalidateShell();
        }
        if (!noMirror_ && !viewportHost_.isMirrorEmbedded() && bridge_->isMirrorRunning()) {
            if (viewportHost_.tryEmbedMirror(mirrorTitle_, bridge_->mirrorProcessId(), pw, ph)) {
                sawEmbeddedMirror_ = true;
                layoutChildren();
                hudOverlay_.showToast("Connected! Click KEYS (F2) to edit outside the phone | " + KeyCodes::toName(profile_.toggleAimVk) + " / F1 for Aim", 3800);
            }
        }
    } else if (demoMode_) {
        viewportHost_.updateVideoLayout(1920, 1080);
    }

    bool showHud = !minimized && (connected || demoMode_);
    RECT vidScr = viewportHost_.activeVideoScreenRect();
    hudOverlay_.syncBounds(vidScr, showHud);
    if (aimMode_ && showHud) {
        updateClipCursor();
    }
}

void ShellWindow::updateClipCursor() {
    if (!aimMode_) return;
    RECT rc = viewportHost_.activeVideoScreenRect();
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    SetCursorPos(cx, cy);
    RECT clipRc = { cx, cy, cx + 1, cy + 1 };
    ClipCursor(&clipRc);
}

bool ShellWindow::isAppForeground() const {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == hwnd_) return true;
    if (hudOverlay_.hwnd() && fg == hudOverlay_.hwnd()) return true;
    if (viewportHost_.hwnd() && fg == viewportHost_.hwnd()) return true;
    if (viewportHost_.embeddedHwnd() && fg == viewportHost_.embeddedHwnd()) return true;
    return false;
}

bool ShellWindow::isPointInVideoScreen(POINT screenPt, float& outNormX, float& outNormY) const {
    RECT rc = viewportHost_.activeVideoScreenRect();
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    if (screenPt.x < rc.left || screenPt.x >= rc.right ||
        screenPt.y < rc.top  || screenPt.y >= rc.bottom) {
        return false;
    }
    outNormX = static_cast<float>(screenPt.x - rc.left) / static_cast<float>(w);
    outNormY = static_cast<float>(screenPt.y - rc.top)  / static_cast<float>(h);
    return true;
}

void ShellWindow::refreshAdbDevices() {
    adbDevices_.clear();
    if (!toolsReady_) return;
    bridge_->listDevices(tools_, adbDevices_);
    if (adbDevices_.empty()) {
        hubStatusMsg_ = "Plug in your Android phone (USB Debugging ON) or use Wireless ADB below.";
    } else {
        hubStatusMsg_.clear();
    }
    invalidateShell();
}

DWORD WINAPI ShellWindow::connectThreadProc(LPVOID param) {
    ShellWindow* self = static_cast<ShellWindow*>(param);
    std::string err;
    bool ok = self->bridge_->connectControl(self->tools_, self->profile_.controlMaxSize, err,
                                            self->pendingSerial_, self->settings_.stayAwake);
    if (ok) {
        if (self->settings_.turnScreenOff) {
            self->bridge_->setDisplayPower(false);
        }
        if (!self->noMirror_) {
            std::string mErr;
            std::string args = self->settings_.buildScrcpyArgs();
            if (!self->bridge_->launchMirrorWindow(self->tools_, args, self->mirrorTitle_, mErr, true, self->bridge_->activeSerial())) {
                self->bridge_->appendLog("[Mirror] " + mErr);
            }
        }
    }
    self->asyncResultOk_ = ok;
    self->asyncResultMsg_ = ok ? ("Connected to " + self->bridge_->deviceName()) : err;
    InterlockedExchange(&self->connecting_, 0);
    if (self->hwnd_) PostMessageA(self->hwnd_, MSG_CONNECT_DONE, 0, 0);
    return 0;
}

void ShellWindow::startAsyncConnect(const std::string& serial) {
    if (!toolsReady_ || connecting_) return;
    if (connectThread_) {
        WaitForSingleObject(connectThread_, 1000);
        CloseHandle(connectThread_);
        connectThread_ = NULL;
    }
    setAimMode(false);
    viewportHost_.detachMirror();
    bridge_->disconnect();
    demoMode_ = false;
    sawEmbeddedMirror_ = false;
    pendingSerial_ = serial;
    hubStatusMsg_ = "Connecting to device & launching embedded stream...";
    InterlockedExchange(&connecting_, 1);
    connectThread_ = CreateThread(NULL, 0, connectThreadProc, this, 0, NULL);
    invalidateShell();
}

DWORD WINAPI ShellWindow::wirelessThreadProc(LPVOID param) {
    ShellWindow* self = static_cast<ShellWindow*>(param);
    std::string msg;
    bool ok = self->bridge_->connectWireless(self->tools_, self->pendingWirelessIp_, msg);
    self->asyncResultOk_ = ok;
    self->asyncResultMsg_ = msg;
    InterlockedExchange(&self->connecting_, 0);
    if (self->hwnd_) PostMessageA(self->hwnd_, MSG_WIRELESS_DONE, 0, 0);
    return 0;
}

void ShellWindow::startAsyncWirelessConnect(const std::string& ipPort) {
    if (!toolsReady_ || connecting_ || ipPort.empty()) return;
    if (connectThread_) {
        WaitForSingleObject(connectThread_, 1000);
        CloseHandle(connectThread_);
        connectThread_ = NULL;
    }
    settings_.lastWirelessAddr = ipPort;
    saveSettings();
    pendingWirelessIp_ = ipPort;
    hubStatusMsg_ = "Connecting Wireless ADB to " + ipPort + "...";
    InterlockedExchange(&connecting_, 1);
    connectThread_ = CreateThread(NULL, 0, wirelessThreadProc, this, 0, NULL);
    invalidateShell();
}

void ShellWindow::disconnectDevice(bool returnToHub) {
    setAimMode(false);
    engine_->releaseAllTouches();
    viewportHost_.detachMirror();
    bridge_->disconnect();
    sawEmbeddedMirror_ = false;
    if (returnToHub) {
        demoMode_ = false;
        setEditMode(false);
        refreshAdbDevices();
    }
    layoutChildren();
    invalidateShell();
}

void ShellWindow::takeScreenshot() {
    if (!bridge_->isConnected()) return;
    std::string dir = exeDir_ + "\\screenshots";
    CreateDirectoryA(dir.c_str(), NULL);
    std::time_t now = std::time(NULL);
    std::tm* tmv = std::localtime(&now);
    char fname[64];
    std::snprintf(fname, sizeof(fname), "phantom_%04d%02d%02d_%02d%02d%02d.png",
                  tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                  tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
    std::string fullPath = dir + "\\" + fname;
    std::string err;
    if (bridge_->captureScreenshot(fullPath, err)) {
        hudOverlay_.showToast(std::string("Screenshot saved -> screenshots\\") + fname, 2400);
    } else {
        hudOverlay_.showToast("Screenshot failed: " + err, 2400);
    }
}

void ShellWindow::setAimMode(bool enabled) {
    if (enabled && !bridge_->isConnected() && !demoMode_) return;
    if (enabled && editMode_) setEditMode(false);
    if (aimMode_ == enabled) return;

    aimMode_ = enabled;
    engine_->setAimMode(enabled);
    cursorDirectDragging_ = false;

    if (aimMode_) {
        POINT curPt;
        if (GetCursorPos(&curPt)) {
            float tx = 0.0f, ty = 0.0f;
            if (isPointInVideoScreen(curPt, tx, ty)) {
                lastUnlockedCursorPt_ = curPt;
                hasLastUnlockedCursorPt_ = true;
            }
        }
        if (hwnd_) SetForegroundWindow(hwnd_);
        updateClipCursor();
        if (!cursorHidden_) { ShowCursor(FALSE); cursorHidden_ = true; }
        SetCursor(NULL);
        hudOverlay_.showToast("AIM LOCK ON  (" + KeyCodes::toName(profile_.toggleAimVk) + " or F1 to unlock)", 1200);
    } else {
        ClipCursor(NULL);
        if (profile_.rememberCursorPos && hasLastUnlockedCursorPt_) {
            float tx = 0.0f, ty = 0.0f;
            if (isPointInVideoScreen(lastUnlockedCursorPt_, tx, ty)) {
                SetCursorPos(lastUnlockedCursorPt_.x, lastUnlockedCursorPt_.y);
            }
        }
        if (cursorHidden_) { ShowCursor(TRUE); cursorHidden_ = false; }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        hudOverlay_.showToast("CURSOR MODE  (KB & WASD stay active!)", 1200);
    }
    hudOverlay_.invalidate();
    invalidateShell();
}

void ShellWindow::setDrawerState(bool open, int tab) {
    if (open && fullscreen_) toggleFullscreen();
    if (open && aimMode_) setAimMode(false);

    drawerOpen_ = open;
    if (open) {
        drawerPanel_.setActiveTab(tab);
        drawerPanel_.setProfilesDir(profilesDir_, profilePath_);
    } else {
        drawerPanel_.clearFocus();
    }

    // Enable interactive keymap editing on the phone canvas whenever Studio Drawer is open
    // on KEYS (0), SENS (1), or PROFILES (2); only DEVICE (3) keeps phone directly clickable!
    bool wantEdit = (drawerOpen_ && drawerPanel_.activeTab() != 3 && (bridge_->isConnected() || demoMode_));
    setEditMode(wantEdit);
    layoutChildren();
}

void ShellWindow::setEditMode(bool enabled) {
    if (enabled && aimMode_) setAimMode(false);
    if (editMode_ == enabled) return;

    editMode_ = enabled;
    if (editMode_) {
        engine_->releaseAllTouches();
    } else {
        hudOverlay_.cancelRebind();
        if (profileDirty_) saveCurrentProfile(true);
    }
    hudOverlay_.setInteractiveEdit(editMode_, profile_.hudOpacity);
    invalidateShell();
}

void ShellWindow::toggleFullscreen() {
    if (!hwnd_) return;
    if (!fullscreen_) {
        if (drawerOpen_) setDrawerState(false, drawerPanel_.activeTab());
        preFullscreenMax_ = (IsZoomed(hwnd_) != FALSE);
        GetWindowRect(hwnd_, &preFullscreenRect_);

        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
        GetMonitorInfoA(mon, &mi);

        fullscreen_ = true;
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        hudOverlay_.showToast("Fullscreen Mode (Press F11 to exit)", 1800);
    } else {
        fullscreen_ = false;
        HWND zOrder = settings_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST;
        if (preFullscreenMax_) {
            SetWindowPos(hwnd_, zOrder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
            ShowWindow(hwnd_, SW_MAXIMIZE);
        } else {
            SetWindowPos(hwnd_, zOrder,
                         preFullscreenRect_.left, preFullscreenRect_.top,
                         preFullscreenRect_.right - preFullscreenRect_.left,
                         preFullscreenRect_.bottom - preFullscreenRect_.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
    }
    layoutChildren();
}

void ShellWindow::toggleAlwaysOnTop() {
    settings_.alwaysOnTop = !settings_.alwaysOnTop;
    saveSettings();
    if (hwnd_ && !fullscreen_) {
        SetWindowPos(hwnd_, settings_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    hudOverlay_.showToast(settings_.alwaysOnTop ? "Window Pinned: Always on Top" : "Window Unpinned", 1400);
    invalidateShell();
}

void ShellWindow::toggleFireMode() {
    profile_.fireMode = (profile_.fireMode + 1) % 3;
    applyProfileToEngine(true);
    const char* msg = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) ? "Fire Mode: DRAG-FIRE (Right)"
                    : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)  ? "Fire Mode: CLAW-LEFT (Separate)"
                                                                  : "Fire Mode: DUAL-DRAG";
    hudOverlay_.showToast(msg, 1800);
}

void ShellWindow::toggleHud() {
    profile_.showHudInPlay = !profile_.showHudInPlay;
    applyProfileToEngine(true);
    hudOverlay_.showToast(profile_.showHudInPlay ? "HUD Key Badges: VISIBLE" : "HUD Key Badges: HIDDEN", 1200);
}

void ShellWindow::triggerQuickReset() {
    engine_->releaseAllTouches();
    hudOverlay_.showToast("All Virtual Touches Reset!", 1000);
}

// --- IHudOverlayHost callbacks ---

std::string ShellWindow::hudSpecialRebindLabel() const {
    if (drawerPanel_.isRebindingToggleAim()) return "AIM LOCK / UNLOCK";
    if (drawerPanel_.isRebindingHoldUnlock()) return "HOLD-TO-UNLOCK CURSOR";
    return "KEY";
}

void ShellWindow::hudOnSelectionChanged(int newSelectedIdx) {
    // Clicking a key node while on PROFILES tab immediately opens its Inspector in KEYS tab!
    if (newSelectedIdx >= 0 && drawerOpen_ && drawerPanel_.activeTab() == 2) {
        drawerPanel_.setActiveTab(0);
    }
    invalidateShell();
}

void ShellWindow::hudOnProfileChanged(bool commitSaveNow) {
    applyProfileToEngine(commitSaveNow);
}

void ShellWindow::hudOnRequestSave() {
    saveCurrentProfile(false);
}

void ShellWindow::hudOnRequestCloseEdit() {
    setDrawerState(false, drawerPanel_.activeTab());
}

void ShellWindow::hudOnMouseBindToDrawer(int mouseVk) {
    DrawerActionResult res;
    if (drawerPanel_.onMouseBind(mouseVk, profile_, res)) {
        handleDrawerResult(res);
    }
}

void ShellWindow::hudOnToast(const std::string& msg, DWORD durationMs) {
    hudOverlay_.showToast(msg, durationMs);
}

void ShellWindow::handleDrawerResult(const DrawerActionResult& res) {
    if (!res.toastMessage.empty()) {
        hudOverlay_.showToast(res.toastMessage, 2200);
    }
    if (res.cancelRebind) {
        hudOverlay_.cancelRebind();
    }
    if (res.profileModified) {
        applyProfileToEngine(!drawerPanel_.isDraggingSlider());
    }
    if (res.settingsModified) {
        saveSettings();
        // Apply Turn Phone Screen Off immediately via control socket without restarting video!
        if (bridge_->isConnected()) {
            bridge_->setDisplayPower(!settings_.turnScreenOff);
        }
    }
    if (res.requestSave) {
        saveCurrentProfile(false);
    }
    if (res.requestSwitchProfile && !res.switchProfilePath.empty()) {
        if (profileDirty_ && lstrcmpiA(profilePath_.c_str(), res.switchProfilePath.c_str()) != 0) {
            saveCurrentProfile(true);
        }
        Profile loaded;
        if (ProfileStore::load(res.switchProfilePath, loaded)) {
            profilePath_ = res.switchProfilePath;
            profile_ = loaded;
            profileDirty_ = false;
            hudOverlay_.setSelectedBtnIndex(-1);
            hudOverlay_.cancelRebind();
            ProfileStore::saveLastActive(profilesDir_, profilePath_);
            engine_->setProfile(profile_);
            hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
            drawerPanel_.setProfilesDir(profilesDir_, profilePath_);
            hudOverlay_.showToast("Loaded config: " + profile_.name, 2000);
        }
    }
    if (res.requestAddNode) {
        KeyNode n;
        n.label = res.addNodeLabel.size() > 7 ? res.addNodeLabel.substr(0, 7) : res.addNodeLabel;
        n.vk = 'K';
        n.action = res.addNodeType;
        float offset = static_cast<float>((addNodeStagger_++ % 6) * 0.03f);
        n.x = clamp01(0.46f + offset);
        n.y = clamp01(0.54f + offset * 0.6f);
        n.x2 = (n.action == NODE_SWIPE) ? clamp01(n.x + 0.14f) : n.x;
        n.y2 = n.y;
        n.durationMs = (n.action == NODE_SWIPE) ? 120 : 35;
        n.radius = 0.030f;
        profile_.buttons.push_back(n);
        int newIdx = static_cast<int>(profile_.buttons.size()) - 1;
        hudOverlay_.setSelectedBtnIndex(newIdx);
        hudOverlay_.startRebind(HUD_REBIND_BUTTON);
        applyProfileToEngine(true);
        hudOverlay_.showToast("Added " + ProfileDefaults::actionTypeToName(n.action) + "! Press ANY Key, Mouse Btn, or Scroll.", 3200);
    }
    if (res.requestStartRebindNode && hudOverlay_.selectedBtnIndex() >= 0 &&
        hudOverlay_.selectedBtnIndex() < static_cast<int>(profile_.buttons.size())) {
        hudOverlay_.startRebind(HUD_REBIND_BUTTON);
        hudOverlay_.showToast("Press ANY Key, Mouse Btn (LMB..X2), Scroll, or Combo...", 3200);
    }
    if (res.requestDeleteSelectedNode && hudOverlay_.selectedBtnIndex() >= 0 &&
        hudOverlay_.selectedBtnIndex() < static_cast<int>(profile_.buttons.size())) {
        profile_.buttons.erase(profile_.buttons.begin() + hudOverlay_.selectedBtnIndex());
        hudOverlay_.setSelectedBtnIndex(-1);
        hudOverlay_.cancelRebind();
        applyProfileToEngine(true);
        hudOverlay_.showToast("Key deleted", 1200);
    }
    if (res.requestCloseDrawer) {
        setDrawerState(false, drawerPanel_.activeTab());
    }
    if (res.requestReconnectStream) {
        saveSettings();
        std::string ser = bridge_->activeSerial();
        startAsyncConnect(ser);
    }
    if (res.requestDisconnectDevice) {
        disconnectDevice(true);
    }
    if (res.requestConnectWireless) {
        startAsyncWirelessConnect(res.wirelessAddr);
    }

    // Keep Edit Mode synced when switching drawer tabs inside the drawer!
    bool wantEdit = (drawerOpen_ && drawerPanel_.activeTab() != 3 && (bridge_->isConnected() || demoMode_));
    if (wantEdit != editMode_) {
        setEditMode(wantEdit);
    }

    hudOverlay_.invalidate();
    invalidateShell();
}

void ShellWindow::handleTopBarAction(TopBarAction act) {
    switch (act) {
        case TB_OPEN_PROFILES:
            if (drawerOpen_ && drawerPanel_.activeTab() == 2) setDrawerState(false, 2);
            else setDrawerState(true, 2);
            break;
        case TB_TOGGLE_AIM: setAimMode(!aimMode_); break;
        case TB_TOGGLE_PIN: toggleAlwaysOnTop(); break;
        case TB_MINIMIZE:   ShowWindow(hwnd_, SW_MINIMIZE); break;
        case TB_MAXIMIZE:
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case TB_CLOSE:
            PostMessageA(hwnd_, WM_CLOSE, 0, 0);
            break;
        default: break;
    }
}

void ShellWindow::handleSideRailAction(SideRailAction act) {
    auto toggleTab = [&](int tab) {
        if (drawerOpen_ && drawerPanel_.activeTab() == tab) setDrawerState(false, tab);
        else setDrawerState(true, tab);
    };
    switch (act) {
        case SR_AIM_TOGGLE:      setAimMode(!aimMode_); break;
        case SR_DRAWER_KEYS:     toggleTab(0); break;
        case SR_DRAWER_SENS:     toggleTab(1); break;
        case SR_DRAWER_PROFILES: toggleTab(2); break;
        case SR_DRAWER_DEVICE:   toggleTab(3); break;
        case SR_HUD_TOGGLE:      toggleHud(); break;
        case SR_FIRE_CYCLE:      toggleFireMode(); break;
        case SR_RESET_TOUCH:     triggerQuickReset(); break;
        case SR_FULLSCREEN:      toggleFullscreen(); break;
        case SR_ANDROID_BACK:    bridge_->sendAndroidNav(4); break;
        case SR_ANDROID_HOME:    bridge_->sendAndroidNav(3); break;
        case SR_ANDROID_RECENTS: bridge_->sendAndroidNav(187); break;
        case SR_ANDROID_VOL_UP:  bridge_->sendAndroidNav(24); break;
        case SR_ANDROID_VOL_DN:  bridge_->sendAndroidNav(25); break;
        case SR_SCREENSHOT:      takeScreenshot(); break;
        default: break;
    }
}

// --- Low-level Hooks (identical non-blocking low-latency path to InputEngine!) ---

LRESULT ShellWindow::handleLowLevelMouse(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (editMode_ || (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind())) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    int mouseVk = 0;
    bool mouseDown = false;
    bool mouseUp = false;
    switch (wParam) {
        case WM_LBUTTONDOWN: mouseVk = VK_LBUTTON; mouseDown = true; break;
        case WM_LBUTTONUP:   mouseVk = VK_LBUTTON; mouseUp = true; break;
        case WM_RBUTTONDOWN: mouseVk = VK_RBUTTON; mouseDown = true; break;
        case WM_RBUTTONUP:   mouseVk = VK_RBUTTON; mouseUp = true; break;
        case WM_MBUTTONDOWN: mouseVk = VK_MBUTTON; mouseDown = true; break;
        case WM_MBUTTONUP:   mouseVk = VK_MBUTTON; mouseUp = true; break;
        case WM_XBUTTONDOWN: mouseVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; mouseDown = true; break;
        case WM_XBUTTONUP:   mouseVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2; mouseUp = true; break;
        case WM_MOUSEWHEEL: {
            short delta = static_cast<short>(HIWORD(ms->mouseData));
            mouseVk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
            mouseDown = true;
            break;
        }
        default: break;
    }

    if (mouseVk != 0 && (aimMode_ || holdUnlockedAim_ || isAppForeground())) {
        if (profile_.holdUnlockVk != 0 && mouseVk == profile_.holdUnlockVk) {
            if (mouseDown && aimMode_ && !holdUnlockedAim_) {
                holdUnlockedAim_ = true;
                PostMessageA(hwnd_, MSG_UNLOCK_AIM, 0, 0);
                return 1;
            }
            if (mouseUp && holdUnlockedAim_) {
                holdUnlockedAim_ = false;
                PostMessageA(hwnd_, MSG_LOCK_AIM, 0, 0);
                return 1;
            }
        }
        if (mouseVk == profile_.toggleAimVk && (bridge_->isConnected() || demoMode_)) {
            if (mouseDown) { PostMessageA(hwnd_, MSG_TOGGLE_AIM, 0, 0); return 1; }
            if (mouseUp) return 1;
        }
    }

    if (aimMode_) {
        if (!isAppForeground()) return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
        if (mouseVk != 0) {
            bool reqToggle = false;
            engine_->onKeyEvent(mouseVk, mouseDown, reqToggle);
            if (reqToggle) PostMessageA(hwnd_, MSG_TOGGLE_AIM, 0, 0);
            return 1;
        }
    } else if (bridge_->isConnected()) {
        float nx = 0.0f, ny = 0.0f;
        bool inVideo = isPointInVideoScreen(ms->pt, nx, ny);

        // Mouse-bound CURSOR_TOGGLE only fires inside video rect (never blocks sidebar/drawer clicks)
        if (inVideo && mouseVk != 0 && mouseVk != VK_LBUTTON && mouseDown && isAppForeground()) {
            bool reqToggle = false;
            bool consumed = engine_->onKeyEvent(mouseVk, true, reqToggle);
            if (reqToggle) PostMessageA(hwnd_, MSG_TOGGLE_AIM, 0, 0);
            if (consumed) return 1;
        }

        if (wParam == WM_LBUTTONDOWN && inVideo && isAppForeground()) {
            cursorDirectDragging_ = true;
            engine_->onDirectCursorTouch(TOUCH_DOWN, nx, ny);
            return 1;
        } else if (wParam == WM_MOUSEMOVE && cursorDirectDragging_) {
            if (inVideo) engine_->onDirectCursorTouch(TOUCH_MOVE, nx, ny);
        } else if (wParam == WM_LBUTTONUP && cursorDirectDragging_) {
            cursorDirectDragging_ = false;
            engine_->onDirectCursorTouch(TOUCH_UP, clamp01(nx), clamp01(ny));
            return 1;
        } else if (wParam == WM_MOUSEWHEEL && inVideo && isAppForeground()) {
            short rawDelta = static_cast<short>(HIWORD(ms->mouseData));
            int notches = rawDelta / WHEEL_DELTA;
            if (notches == 0 && rawDelta != 0) notches = (rawDelta > 0) ? 1 : -1;
            if (notches != 0) {
                engine_->onDirectCursorScroll(nx, ny, notches);
                return 1;
            }
        }
    }

    return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
}

LRESULT ShellWindow::handleLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    int vk = static_cast<int>(kb->vkCode);
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!down && !up) return CallNextHookEx(keyHook_, nCode, wParam, lParam);

    bool isRepeat = false;
    if (vk >= 0 && vk < 256) {
        if (down) { isRepeat = hookKeyDown_[vk]; hookKeyDown_[vk] = true; }
        else if (up) { hookKeyDown_[vk] = false; }
    }

    if (up && (KeyCodes::matches(vk, profile_.toggleAimVk) || vk == VK_F1)) {
        toggleAimKeyDown_ = false;
        if (!editMode_ && (aimMode_ || isAppForeground())) return 1;
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }
    if (up && holdUnlockedAim_ && KeyCodes::matches(vk, profile_.holdUnlockVk)) {
        holdUnlockedAim_ = false;
        if (!editMode_ && isAppForeground()) {
            PostMessageA(hwnd_, MSG_LOCK_AIM, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }
    if (down && (vk == VK_LWIN || vk == VK_RWIN)) {
        if (aimMode_) PostMessageA(hwnd_, MSG_UNLOCK_AIM, 0, 0);
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (!aimMode_ && !isAppForeground()) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    // While typing in any input box (Drawer or Hub) or rebinding a key -> pass straight to Win32 UI!
    bool uiCapturingText = editingHubWireless_ ||
                           (drawerOpen_ && (drawerPanel_.isTypingText() || drawerPanel_.isWaitingSpecialRebind())) ||
                           (hudOverlay_.rebindTarget() != HUD_REBIND_NONE);
    if (editMode_ || uiCapturingText) {
        if (down && !isRepeat && vk == VK_F2 && !uiCapturingText) {
            PostMessageA(hwnd_, MSG_TOGGLE_STUDIO, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (!bridge_->isConnected() && !demoMode_) {
        if (down && !isRepeat && vk == VK_F11) { PostMessageA(hwnd_, MSG_TOGGLE_FULLSCREEN, 0, 0); return 1; }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (KeyCodes::matches(vk, profile_.holdUnlockVk)) {
        if (down) {
            if (!isRepeat && aimMode_ && !holdUnlockedAim_) {
                holdUnlockedAim_ = true;
                PostMessageA(hwnd_, MSG_UNLOCK_AIM, 0, 0);
            }
            if (holdUnlockedAim_) return 1;
        }
    }

    if (KeyCodes::matches(vk, profile_.toggleAimVk) || vk == VK_F1) {
        if (down) {
            if (!isRepeat && !toggleAimKeyDown_) {
                toggleAimKeyDown_ = true;
                PostMessageA(hwnd_, MSG_TOGGLE_AIM, 0, 0);
            }
            return 1;
        }
    }

    if (down && isRepeat) {
        if (aimMode_ || engine_->isKeyMapped(vk, false)) return 1;
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (down) {
        if (vk == VK_F2)  { PostMessageA(hwnd_, MSG_TOGGLE_STUDIO, 0, 0); return 1; }
        if (vk == VK_F3)  { PostMessageA(hwnd_, MSG_CYCLE_FIRE, 0, 0); return 1; }
        if (vk == VK_F4)  { PostMessageA(hwnd_, MSG_TOGGLE_HUD, 0, 0); return 1; }
        if (vk == VK_F11) { PostMessageA(hwnd_, MSG_TOGGLE_FULLSCREEN, 0, 0); return 1; }
        if (vk == profile_.resetTouchVk) { PostMessageA(hwnd_, MSG_RESET_TOUCH, 0, 0); return 1; }
        if (vk == VK_OEM_4)     { PostMessageA(hwnd_, MSG_SENS_X_DOWN, 0, 0); return 1; }
        if (vk == VK_OEM_6)     { PostMessageA(hwnd_, MSG_SENS_X_UP, 0, 0); return 1; }
        if (vk == VK_OEM_MINUS) { PostMessageA(hwnd_, MSG_SENS_Y_DOWN, 0, 0); return 1; }
        if (vk == VK_OEM_PLUS)  { PostMessageA(hwnd_, MSG_SENS_Y_UP, 0, 0); return 1; }
    }

    bool reqToggle = false;
    bool consumed = engine_->onKeyEvent(vk, down, reqToggle);
    if (reqToggle) PostMessageA(hwnd_, MSG_TOGGLE_AIM, 0, 0);
    if (consumed) return 1;

    return CallNextHookEx(keyHook_, nCode, wParam, lParam);
}

// --- Win32 Window Procedure ---

LRESULT CALLBACK ShellWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ShellWindow* self = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = reinterpret_cast<ShellWindow*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ShellWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT ShellWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE: {
            if (wParam == TRUE) {
                if (IsZoomed(hwnd) && !fullscreen_) {
                    NCCALCSIZE_PARAMS* ncp = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi;
                    ZeroMemory(&mi, sizeof(mi));
                    mi.cbSize = sizeof(mi);
                    if (GetMonitorInfoA(mon, &mi)) {
                        ncp->rgrc[0] = mi.rcWork;
                    }
                }
                return 0;
            }
            break;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = Theme::MIN_WIN_W;
            mmi->ptMinTrackSize.y = Theme::MIN_WIN_H;
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoA(mon, &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
            return 0;
        }

        case WM_NCHITTEST: {
            if (fullscreen_) return HTCLIENT;
            POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            RECT wr;
            GetWindowRect(hwnd, &wr);
            if (!IsZoomed(hwnd)) {
                const int b = Theme::RESIZE_BORDER;
                bool l = (pt.x < wr.left + b);
                bool r = (pt.x >= wr.right - b);
                bool t = (pt.y < wr.top + b);
                bool bt = (pt.y >= wr.bottom - b);
                if (t && l) return HTTOPLEFT;
                if (t && r) return HTTOPRIGHT;
                if (bt && l) return HTBOTTOMLEFT;
                if (bt && r) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (bt) return HTBOTTOM;
            }
            POINT cpt = pt;
            ScreenToClient(hwnd, &cpt);
            if (ptIn(topBarRc_, cpt.x, cpt.y) && !topBar_.isInteractivePoint(cpt.x, cpt.y)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_SIZE:
        case WM_MOVE:
            layoutChildren();
            return 0;

        case WM_INPUT: {
            if (!aimMode_) break;
            UINT dwSize = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
            if (dwSize > 0 && dwSize <= 256) {
                BYTE rawBuf[256];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, rawBuf, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
                    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(rawBuf);
                    if (raw->header.dwType == RIM_TYPEMOUSE &&
                        (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                        LONG dx = raw->data.mouse.lLastX;
                        LONG dy = raw->data.mouse.lLastY;
                        if (dx != 0 || dy != 0) {
                            engine_->onRawMouseDelta(static_cast<int>(dx), static_cast<int>(dy));
                        }
                    }
                }
            }
            return 0;
        }

        case MSG_TOGGLE_AIM:        setAimMode(!aimMode_); return 0;
        case MSG_UNLOCK_AIM:        setAimMode(false); return 0;
        case MSG_LOCK_AIM:          setAimMode(true); return 0;
        case MSG_TOGGLE_STUDIO:
            if (drawerOpen_ && drawerPanel_.activeTab() == 0) setDrawerState(false, 0);
            else setDrawerState(true, 0);
            return 0;
        case MSG_CYCLE_FIRE:        toggleFireMode(); return 0;
        case MSG_TOGGLE_HUD:        toggleHud(); return 0;
        case MSG_RESET_TOUCH:       triggerQuickReset(); return 0;
        case MSG_TOGGLE_FULLSCREEN: toggleFullscreen(); return 0;

        case MSG_CONNECT_DONE: {
            hubStatusMsg_ = asyncResultOk_ ? "" : asyncResultMsg_;
            if (!asyncResultOk_) refreshAdbDevices();
            layoutChildren();
            invalidateShell();
            return 0;
        }
        case MSG_WIRELESS_DONE: {
            hubStatusMsg_ = asyncResultMsg_;
            refreshAdbDevices();
            if (asyncResultOk_) {
                startAsyncConnect(pendingWirelessIp_);
            } else {
                hudOverlay_.showToast(asyncResultMsg_, 2600);
            }
            invalidateShell();
            return 0;
        }

        case MSG_SENS_X_DOWN:
            profile_.sensX = clampRange(profile_.sensX - 0.05f, 0.10f, 10.0f);
            applyProfileToEngine(true);
            { char b[64]; std::snprintf(b, sizeof(b), "Sens X: %.2f  (Y: %.2f)", profile_.sensX, profile_.sensY); hudOverlay_.showToast(b, 1200); }
            return 0;
        case MSG_SENS_X_UP:
            profile_.sensX = clampRange(profile_.sensX + 0.05f, 0.10f, 10.0f);
            applyProfileToEngine(true);
            { char b[64]; std::snprintf(b, sizeof(b), "Sens X: %.2f  (Y: %.2f)", profile_.sensX, profile_.sensY); hudOverlay_.showToast(b, 1200); }
            return 0;
        case MSG_SENS_Y_DOWN:
            profile_.sensY = clampRange(profile_.sensY - 0.05f, 0.10f, 10.0f);
            applyProfileToEngine(true);
            { char b[64]; std::snprintf(b, sizeof(b), "Sens Y: %.2f  (X: %.2f)", profile_.sensY, profile_.sensX); hudOverlay_.showToast(b, 1200); }
            return 0;
        case MSG_SENS_Y_UP:
            profile_.sensY = clampRange(profile_.sensY + 0.05f, 0.10f, 10.0f);
            applyProfileToEngine(true);
            { char b[64]; std::snprintf(b, sizeof(b), "Sens Y: %.2f  (X: %.2f)", profile_.sensY, profile_.sensX); hudOverlay_.showToast(b, 1200); }
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_TICK) {
                bool fg = isAppForeground();
                if (!fg) {
                    if (aimMode_) setAimMode(false);
                    if (hadForeground_) engine_->releaseAllTouches();
                }
                hadForeground_ = fg;

                // Detect if embedded phone mirror disconnected externally (e.g. cable unplugged)
                if (sawEmbeddedMirror_ && !bridge_->isConnected() && !connecting_) {
                    disconnectDevice(true);
                    hubStatusMsg_ = "Phone disconnected. Reconnect USB cable and click Refresh.";
                }

                syncHudAndVideo();
                hudOverlay_.checkToastExpired();

                DWORD nowTick = GetTickCount();
                if ((nowTick - lastTelemetryPaintTick_) >= 150) {
                    lastTelemetryPaintTick_ = nowTick;
                    if (!editMode_ && (bridge_->isConnected() || demoMode_)) {
                        hudOverlay_.invalidate();
                    }
                    // Lightweight refresh of TopBar live telemetry pill
                    if (!fullscreen_) {
                        InvalidateRect(hwnd, &topBarRc_, FALSE);
                    }
                }
            }
            return 0;

        case WM_MOUSEMOVE: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            hoverX_ = mx;
            hoverY_ = my;
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            if (drawerOpen_ && drawerPanel_.isDraggingSlider()) {
                DrawerActionResult res;
                if (drawerPanel_.onMouseMove(mx, my, profile_, hudOverlay_.selectedBtnIndex(), res)) {
                    handleDrawerResult(res);
                }
            }
            invalidateShell();
            return 0;
        }

        case WM_MOUSELEAVE:
            hoverX_ = -1;
            hoverY_ = -1;
            invalidateShell();
            return 0;

        case WM_LBUTTONDOWN: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            SetFocus(hwnd);
            editingHubWireless_ = false;

            // 1. TopBar interactive controls
            if (ptIn(topBarRc_, mx, my)) {
                TopBarAction tb = topBar_.hitTest(mx, my);
                if (tb != TB_NONE) { handleTopBarAction(tb); return 0; }
            }
            // 2. SideRail emulator buttons
            if (ptIn(railRc_, mx, my)) {
                SideRailAction sr = sideRail_.hitTest(mx, my);
                if (sr != SR_NONE) { handleSideRailAction(sr); return 0; }
            }
            // 3. Docked Outside-Viewport DrawerPanel
            if (drawerOpen_ && drawerPanel_.containsPoint(mx, my)) {
                SetCapture(hwnd);
                int sel = hudOverlay_.selectedBtnIndex();
                DrawerActionResult res;
                if (drawerPanel_.onMouseDown(mx, my, profile_, settings_, sel, res)) {
                    hudOverlay_.setSelectedBtnIndex(sel);
                    handleDrawerResult(res);
                    return 0;
                }
            }
            // 4. Device Hub inside ViewportHost (when disconnected)
            if (!bridge_->isConnected() && !demoMode_ && ptIn(viewportRc_, mx, my)) {
                HubClickResult hr = viewportHost_.hitTestHub(mx, my);
                if (hr.action == HUB_REFRESH_DEVICES) {
                    refreshAdbDevices();
                } else if (hr.action == HUB_CONNECT_DEVICE && hr.index >= 0 &&
                           hr.index < static_cast<int>(adbDevices_.size())) {
                    startAsyncConnect(adbDevices_[hr.index].serial);
                } else if (hr.action == HUB_FOCUS_WIFI_ADDR) {
                    editingHubWireless_ = true;
                    invalidateShell();
                } else if (hr.action == HUB_CONNECT_WIFI) {
                    startAsyncWirelessConnect(settings_.lastWirelessAddr);
                } else if (hr.action == HUB_START_OFFLINE_STUDIO) {
                    demoMode_ = true;
                    setDrawerState(true, 0);
                    hudOverlay_.showToast("Offline Keymap Studio active! Changes auto-save to " + profile_.name, 3000);
                }
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (GetCapture() == hwnd) ReleaseCapture();
            if (drawerOpen_ && drawerPanel_.onMouseUp()) {
                applyProfileToEngine(true);
            }
            return 0;
        }

        case WM_MBUTTONDOWN:
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) {
                hudOnMouseBindToDrawer(VK_MBUTTON);
                return 0;
            }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE && hudOverlay_.handleMouseBind(VK_MBUTTON)) return 0;
            break;

        case WM_XBUTTONDOWN: {
            int xvk = (HIWORD(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) { hudOnMouseBindToDrawer(xvk); return 0; }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE && hudOverlay_.handleMouseBind(xvk)) return 0;
            break;
        }

        case WM_RBUTTONDOWN:
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) { hudOnMouseBindToDrawer(VK_RBUTTON); return 0; }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE && hudOverlay_.handleMouseBind(VK_RBUTTON)) return 0;
            break;

        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) {
                hudOnMouseBindToDrawer((delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN);
                return 0;
            }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE) {
                if (hudOverlay_.handleMouseBind((delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN)) return 0;
            }
            POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            ScreenToClient(hwnd, &pt);
            if (drawerOpen_ && drawerPanel_.onMouseWheel(pt.x, pt.y, delta)) {
                invalidateShell();
                return 0;
            }
            break;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = static_cast<int>(wParam);
            if (editingHubWireless_) {
                if (vk == VK_RETURN) {
                    editingHubWireless_ = false;
                    startAsyncWirelessConnect(settings_.lastWirelessAddr);
                } else if (vk == VK_ESCAPE) {
                    editingHubWireless_ = false;
                } else if (vk == VK_BACK && !settings_.lastWirelessAddr.empty()) {
                    settings_.lastWirelessAddr.pop_back();
                    saveSettings();
                }
                invalidateShell();
                return 0;
            }
            if (drawerOpen_) {
                DrawerActionResult res;
                if (drawerPanel_.onKeyDown(vk, profile_, settings_, hudOverlay_.selectedBtnIndex(), res)) {
                    handleDrawerResult(res);
                    return 0;
                }
            }
            if (editMode_ && hudOverlay_.handleKeyDown(vk)) return 0;
            if (!editMode_ && msg == WM_SYSKEYDOWN) return 0;
            break;
        }

        case WM_CHAR: {
            char ch = static_cast<char>(wParam);
            if (editingHubWireless_ && ch >= 32 && ch <= 126) {
                if (settings_.lastWirelessAddr.size() < 28) {
                    settings_.lastWirelessAddr.push_back(ch);
                    saveSettings();
                    invalidateShell();
                }
                return 0;
            }
            if (drawerOpen_) {
                DrawerActionResult res;
                if (drawerPanel_.onChar(ch, profile_, settings_, hudOverlay_.selectedBtnIndex(), res)) {
                    handleDrawerResult(res);
                    return 0;
                }
            }
            break;
        }

        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (!editMode_) return 0;
            break;

        case WM_SYSCOMMAND:
            if (!editMode_ && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
            break;

        case WM_SETCURSOR:
            if (aimMode_) { SetCursor(NULL); return TRUE; }
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            onPaint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            flushOnExit();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            flushOnExit();
            setAimMode(false);
            viewportHost_.detachMirror();
            KillTimer(hwnd, TIMER_TICK);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void ShellWindow::onPaint(HDC hdc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (!cacheDC_ || w != cacheW_ || h != cacheH_) {
        if (cacheDC_) {
            if (cacheOldBmp_) SelectObject(cacheDC_, cacheOldBmp_);
            if (cacheBmp_) DeleteObject(cacheBmp_);
            DeleteDC(cacheDC_);
        }
        cacheDC_ = CreateCompatibleDC(hdc);
        cacheBmp_ = CreateCompatibleBitmap(hdc, w, h);
        cacheOldBmp_ = SelectObject(cacheDC_, cacheBmp_);
        cacheW_ = w;
        cacheH_ = h;
    }

    {
        Gdiplus::Graphics g(cacheDC_);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        bool connected = bridge_->isConnected();
        std::string devLabel = bridge_->deviceName();
        if (connected) {
            char dbuf[96];
            std::snprintf(dbuf, sizeof(dbuf), "%s · %ux%u",
                          devLabel.empty() ? "Android" : devLabel.c_str(),
                          lastPhoneW_, lastPhoneH_);
            devLabel = dbuf;
        }

        // 1. Center Viewport backdrop / Device Hub
        viewportHost_.paint(g, viewportRc_, connected, demoMode_, (connecting_ != 0),
                            hubStatusMsg_, adbDevices_, settings_.lastWirelessAddr,
                            editingHubWireless_, hoverX_, hoverY_);

        if (!fullscreen_) {
            // 2. TopBar
            EngineTelemetry tel = engine_->getTelemetry();
            topBar_.paint(g, topBarRc_, devLabel, connected, demoMode_,
                          profile_, tel, settings_.alwaysOnTop,
                          (IsZoomed(hwnd_) != FALSE), hoverX_, hoverY_);

            // 3. Docked Outside-Viewport DrawerPanel (when open)
            if (drawerOpen_) {
                std::vector<std::string> logs = bridge_->getRecentLogs();
                drawerPanel_.paint(g, drawerRc_, profile_, settings_, profilePath_,
                                   hudOverlay_.selectedBtnIndex(),
                                   (hudOverlay_.rebindTarget() == HUD_REBIND_BUTTON),
                                   connected, devLabel, logs, hoverX_, hoverY_);
            }

            // 4. Right Emulator SideRail
            SideRailState st;
            st.aimMode = aimMode_;
            st.drawerOpen = drawerOpen_;
            st.activeDrawerTab = drawerPanel_.activeTab();
            st.showHud = profile_.showHudInPlay;
            st.fullscreen = fullscreen_;
            st.connected = connected;
            sideRail_.paint(g, railRc_, st, hoverX_, hoverY_);

            // Subtle hover tooltip left of SideRail
            std::string tip = sideRail_.tooltipAt(hoverX_, hoverY_);
            if (!tip.empty() && ptIn(railRc_, hoverX_, hoverY_)) {
                int tw = 22 + static_cast<int>(tip.size()) * 7;
                if (tw < 96) tw = 96;
                if (tw > 230) tw = 230;
                int ty = hoverY_ - 13;
                if (ty < topBarRc_.bottom + 4) ty = topBarRc_.bottom + 4;
                if (ty + 26 > h - 4) ty = h - 30;
                RECT tr = { railRc_.left - tw - 8, ty, railRc_.left - 8, ty + 26 };
                GdiPlusDraw::roundedCard(g, tr, 6.0f, Theme::BG_SHELL, Theme::ACCENT_CYAN, 1.0f);
                GdiPlusDraw::textCenter(g, tip, static_cast<float>(tr.left + tr.right) * 0.5f,
                                        static_cast<float>(tr.top + tr.bottom) * 0.5f,
                                        11.0f, true, Theme::TEXT_PRIMARY);
            }

            // Crisp outer 1px window border when restored
            if (!IsZoomed(hwnd_)) {
                GdiPlusDraw::strokeRoundedRect(g, 0.5f, 0.5f, static_cast<float>(w - 1), static_cast<float>(h - 1), 0.0f, Theme::BORDER_STRONG, 1.0f);
            }
        }
    }

    BitBlt(hdc, 0, 0, w, h, cacheDC_, 0, 0, SRCCOPY);
}
