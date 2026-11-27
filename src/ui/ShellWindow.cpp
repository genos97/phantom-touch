#include "ui/ShellWindow.hpp"
#include "config/KeyCodes.hpp"
#include "config/ProfileStore.hpp"
#include "ui/Strings.hpp"
#include "ui/Theme.hpp"

#include <shellapi.h>
#include <commdlg.h>
#include <imm.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>

#ifndef WM_CLIPBOARDUPDATE
#define WM_CLIPBOARDUPDATE 0x031D
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef HTMAXBUTTON
#define HTMAXBUTTON 9
#endif
#ifndef WM_DEVICECHANGE
#define WM_DEVICECHANGE 0x0219
#endif
#ifndef DBT_DEVICEARRIVAL
#define DBT_DEVICEARRIVAL 0x8000
#endif
#ifndef DBT_DEVICEREMOVECOMPLETE
#define DBT_DEVICEREMOVECOMPLETE 0x8004
#endif
#ifndef DBT_DEVNODES_CHANGED
#define DBT_DEVNODES_CHANGED 0x0007
#endif

namespace {

static const UINT_PTR TIMER_TICK = 1;
static const UINT_PTR TIMER_ANIM = 2;

struct ProfileSaveJob {
    std::string path;
    std::string profilesDir;
    Profile profile;
};

struct SettingsSaveJob {
    std::string path;
    AppSettings settings;
    volatile LONG* inFlightFlag;
};

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

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), NULL, 0);
    if (len <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], len);
    return w;
}

std::string wideToUtf8(const wchar_t* ws) {
    if (!ws || !*ws) return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, ws, -1, NULL, 0, NULL, NULL);
    if (len <= 1) return std::string();
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, -1, &s[0], len - 1, NULL, NULL);
    return s;
}

bool isCfgFilePath(const std::string& p) {
    size_t dot = p.find_last_of('.');
    return (dot != std::string::npos && lstrcmpiA(p.c_str() + dot, ".cfg") == 0);
}

std::string makeUniqueProfilePath(const std::string& dir, const std::string& rawName, std::string& outFname) {
    std::string base = ProfileStore::sanitizeFilename(rawName);
    outFname = base;
    std::string full = dir + "\\" + outFname + ".cfg";
    int suffix = 2;
    while (GetFileAttributesA(full.c_str()) != INVALID_FILE_ATTRIBUTES && suffix < 200) {
        char sbuf[32];
        std::snprintf(sbuf, sizeof(sbuf), "_%d", suffix++);
        outFname = base + sbuf;
        full = dir + "\\" + outFname + ".cfg";
    }
    return full;
}

bool writeClipboardUtf8(HWND owner, const std::string& utf8) {
    std::wstring w = utf8ToWide(utf8);
    if (w.empty() || !OpenClipboard(owner)) return false;
    EmptyClipboard();
    SIZE_T bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }
    void* ptr = GlobalLock(hMem);
    if (ptr) {
        std::memcpy(ptr, w.c_str(), bytes);
        GlobalUnlock(hMem);
    }
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
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
      asyncCtx_(new AsyncContext()),
      hudOverlay_(this),
      demoMode_(startDemo) {
    InitializeCriticalSection(&auxLock_);
    probeEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    deviceMonitorWakeEvent_ = CreateEventW(NULL, FALSE, FALSE, NULL);
    viewportHost_.setProbeFrameEvent(probeEvent_);
    ZeroMemory(&topBarRc_, sizeof(topBarRc_));
    ZeroMemory(&railRc_, sizeof(railRc_));
    ZeroMemory(&drawerRc_, sizeof(drawerRc_));
    ZeroMemory(&viewportRc_, sizeof(viewportRc_));
    ZeroMemory(&preFullscreenRect_, sizeof(preFullscreenRect_));
    ZeroMemory(&lastUnlockedCursorPt_, sizeof(lastUnlockedCursorPt_));

    char tbuf[64];
    std::snprintf(tbuf, sizeof(tbuf), "PhantomTouch-Video-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    mirrorTitle_ = tbuf;

    profile_ = engine_->getProfileCopy();
    AppSettingsStore::load(settingsPath_, settings_);
    Strings::init(exeDir_ + "\\lang", settings_.uiLanguage);
    if (profile_.keymapEnabled) {
        lastGameProfilePath_ = profilePath_;
        settings_.lastGameProfile = profilePath_;
    } else {
        // Restore remembered game profile (e.g. my_custom.cfg) if app launched while on desktop.cfg
        if (!settings_.lastGameProfile.empty()) {
            Profile cand;
            if (ProfileStore::load(settings_.lastGameProfile, cand) && cand.keymapEnabled) {
                lastGameProfilePath_ = settings_.lastGameProfile;
            }
        }
        if (lastGameProfilePath_.empty()) {
            std::vector<std::string> files = ProfileStore::listProfiles(profilesDir_);
            std::string fallbackDefault;
            for (size_t i = 0; i < files.size(); ++i) {
                Profile cand;
                if (ProfileStore::load(files[i], cand) && cand.keymapEnabled) {
                    size_t sl = files[i].find_last_of("\\/");
                    std::string base = (sl == std::string::npos) ? files[i] : files[i].substr(sl + 1);
                    if (lstrcmpiA(base.c_str(), "freefire.cfg") == 0) {
                        fallbackDefault = files[i];
                    } else {
                        lastGameProfilePath_ = files[i];
                        break;
                    }
                }
            }
            if (lastGameProfilePath_.empty()) lastGameProfilePath_ = fallbackDefault;
        }
    }
    lastAppliedScreenOff_ = settings_.turnScreenOff;
    appliedStreamSettings_ = settings_;
    showOnboarding_ = (!settings_.onboardingDone && !demoMode_ && !noMirror_);
    InterlockedExchange(&autoSwitchAtomic_, settings_.autoSwitchProfiles ? 1 : 0);
    InterlockedExchange(&gameProfileAtomic_, profile_.keymapEnabled ? 1 : 0);
    engine_->setGamepadEnabled(settings_.gamepadEnabled);
    drawerPanel_.setProfilesDir(profilesDir_, profilePath_);
    recordRecentProfile(profilePath_);
    initCommandPalette();
    bridge_->setClipboardCallback(ShellWindow::onDeviceClipboard, this);
    bridge_->setConnectStepCallback(ShellWindow::onConnectStep, this);
}

ShellWindow::~ShellWindow() {
    InterlockedExchange(&stopAuxThreads_, 1);
    InterlockedExchange(&stopProbe_, 1);
    if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
    stopQrWirelessPairing();
    if (asyncCtx_) {
        EnterCriticalSection(&asyncCtx_->lock);
        InterlockedExchange(&asyncCtx_->alive, 0);
        asyncCtx_->hwnd = NULL;
        LeaveCriticalSection(&asyncCtx_->lock);
    }
    tooltipPopup_.destroy();
    modalPopup_.destroy();
    if (clipboardListenerActive_ && hwnd_ && IsWindow(hwnd_)) {
        RemoveClipboardFormatListener(hwnd_);
        clipboardListenerActive_ = false;
    }
    bridge_->setClipboardCallback(nullptr, nullptr);
    bridge_->setConnectStepCallback(nullptr, nullptr);
    bridge_->setVideoPacketCallback(nullptr, nullptr);
    bridge_->setAudioPacketCallback(nullptr, nullptr);
    audioPlayer_.requestStop();
    viewportHost_.setProbeFrameEvent(NULL);
    viewportHost_.setProbeSampleRect(false, 0.5f, 0.5f);
    flushOnExit();
    setAimMode(false);
    hookThread_.stop();
    bridge_->disconnect();
    audioPlayer_.stop();
    h264Decoder_.shutdown();
    viewportHost_.detachMirror();
    hudOverlay_.destroy();
    if (probeEvent_) {
        CloseHandle(probeEvent_);
        probeEvent_ = NULL;
    }
    if (deviceMonitorWakeEvent_) {
        CloseHandle(deviceMonitorWakeEvent_);
        deviceMonitorWakeEvent_ = NULL;
    }
    if (cacheDC_) {
        if (cacheOldBmp_) SelectObject(cacheDC_, cacheOldBmp_);
        if (cacheBmp_) DeleteObject(cacheBmp_);
        DeleteDC(cacheDC_);
        cacheDC_ = NULL;
        cacheBmp_ = NULL;
        cacheOldBmp_ = NULL;
    }
    if (asyncCtx_) {
        asyncCtx_->release();
        asyncCtx_ = nullptr;
    }
    DeleteCriticalSection(&auxLock_);
}

bool ShellWindow::create(HINSTANCE hInst) {
    hInst_ = hInst;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = ShellWindow::wndProc;
    wc.hInstance = hInst_;
    wc.hIcon = LoadIconA(hInst_, MAKEINTRESOURCEA(101));
    wc.hIconSm = LoadIconA(hInst_, MAKEINTRESOURCEA(101));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PhantomTouchShellClass";

    if (!RegisterClassExW(&wc)) return false;

    metrics_.dpi = GdiPlusDraw::queryWindowDpi(NULL);
    GdiPlusDraw::setActiveDpi(metrics_.dpi);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int w = settings_.winW;
    int h = settings_.winH;
    if (w < metrics_.minWinW()) w = metrics_.minWinW();
    if (h < metrics_.minWinH()) h = metrics_.minWinH();
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

    hwnd_ = CreateWindowExW(exStyle, wc.lpszClassName, L"Phantom Touch",
                            style, x, y, w, h, NULL, NULL, hInst_, this);
    if (!hwnd_) return false;

    metrics_.dpi = GdiPlusDraw::queryWindowDpi(hwnd_);
    GdiPlusDraw::setActiveDpi(metrics_.dpi);
    GdiPlusDraw::warmCache();

    EnterCriticalSection(&asyncCtx_->lock);
    asyncCtx_->hwnd = hwnd_;
    InterlockedExchange(&asyncCtx_->alive, 1);
    LeaveCriticalSection(&asyncCtx_->lock);

    dwmRoundedActive_ = GdiPlusDraw::enableDarkDwmFrame(hwnd_);
    clipboardListenerActive_ = (AddClipboardFormatListener(hwnd_) != FALSE);
    DragAcceptFiles(hwnd_, TRUE);
    viewportHost_.create(hwnd_, hInst_);
    hudOverlay_.create(hwnd_, hInst_);
    tooltipPopup_.create(hwnd_, hInst_);
    modalPopup_.create(hwnd_, hInst_, this);
    engine_->setPadNavCallback(ShellWindow::onPadNav, this);

    InputHookSnapshot initSnap;
    initSnap.shellHwnd = hwnd_;
    initSnap.hudHwnd = hudOverlay_.hwnd();
    initSnap.viewportHwnd = viewportHost_.hwnd();
    hookThread_.start(hInst_, engine_, initSnap);

    SetTimer(hwnd_, TIMER_TICK, 50, NULL);
    InterlockedExchange(&stopAuxThreads_, 0);
    asyncCtx_->addRef();
    HANDLE hWatcher = CreateThread(NULL, 0, appWatcherThreadProc, new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
    if (hWatcher) CloseHandle(hWatcher);
    asyncCtx_->addRef();
    HANDLE hDevMon = CreateThread(NULL, 0, deviceMonitorThreadProc, new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
    if (hDevMon) CloseHandle(hDevMon);
    else asyncCtx_->release();

    if (demoMode_) {
        drawerOpen_ = true;
        drawerPanel_.setActiveTab(0);
        setEditMode(true);
        layoutChildren();
        showToast("Offline Keymap Studio ready! Edit keys on canvas or in the Right Drawer.", 4000);
    } else {
        hubStatusMsg_ = "Locating scrcpy / ADB tools...";
        layoutChildren();
        // N36: run scrcpy --version and initial ADB device discovery off the UI thread!
        asyncCtx_->addRef();
        HANDLE hTools = CreateThread(NULL, 0, startupToolsThreadProc, new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
        if (hTools) CloseHandle(hTools);
    }

    // S6: Pre-allocate & page-fault the offscreen DIB backing buffer before ShowWindow/WM_PAINT
    if (HDC initDc = GetDC(hwnd_)) {
        int initW = 0, initH = 0;
        if (settings_.maximized) {
            HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            ZeroMemory(&mi, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                initW = mi.rcWork.right - mi.rcWork.left;
                initH = mi.rcWork.bottom - mi.rcWork.top;
            }
        }
        if (initW <= 0 || initH <= 0) {
            RECT cr;
            GetClientRect(hwnd_, &cr);
            initW = cr.right - cr.left;
            initH = cr.bottom - cr.top;
        }
        if (initW > 0 && initH > 0) {
            ensurePaintCache(initDc, initW, initH);
        }
        ReleaseDC(hwnd_, initDc);
    }

    if (autoFullscreen_) {
        toggleFullscreen();
    } else {
        if (settings_.maximized) {
            ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
        } else {
            ShowWindow(hwnd_, SW_SHOW);
        }
        UpdateWindow(hwnd_);
    }
    syncInputHookSnapshot();
    return true;
}

int ShellWindow::runMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void ShellWindow::syncInputHookSnapshot() {
    InputHookSnapshot s;
    s.shellHwnd = hwnd_;
    s.hudHwnd = hudOverlay_.hwnd();
    s.viewportHwnd = viewportHost_.hwnd();
    s.embeddedHwnd = viewportHost_.embeddedHwnd();
    s.videoScreenRect = viewportHost_.activeVideoScreenRect();
    s.connected = bridge_->isConnected();
    s.demoMode = demoMode_;
    s.aimMode = aimMode_;
    s.editMode = editMode_;
    s.nativeVideoActive = viewportHost_.isNativeVideoActive();
    s.uiCapturingText = editingHubWireless_ ||
                        (showOnboarding_ && onboardingView_.isEditingWifi()) ||
                        (drawerOpen_ && (drawerPanel_.isTypingText() || drawerPanel_.hasKeyboardFocus() || drawerPanel_.isWaitingSpecialRebind())) ||
                        (hudOverlay_.rebindTarget() != HUD_REBIND_NONE) ||
                        (activePopup_ != POPUP_NONE);
    s.popupOpen = (activePopup_ != POPUP_NONE);
    s.waitingSpecialRebind = (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind());
    s.keymapEnabled = profile_.keymapEnabled;
    s.rightClickBack = settings_.rightClickBack;
    s.toggleAimVk = profile_.toggleAimVk;
    s.holdUnlockVk = profile_.holdUnlockVk;
    s.resetTouchVk = profile_.resetTouchVk;
    hookThread_.updateSnapshot(s);
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

DWORD WINAPI ShellWindow::settingsSaveWorkerProc(LPVOID param) {
    SettingsSaveJob* job = static_cast<SettingsSaveJob*>(param);
    if (job) {
        AppSettingsStore::save(job->path, job->settings);
        if (job->inFlightFlag) InterlockedExchange(job->inFlightFlag, 0);
        delete job;
    }
    return 0;
}

void ShellWindow::scheduleSettingsSave() {
    saveWindowPlacement();
    settingsSaveDueTick_ = GetTickCount() + 450;
    hasPendingSettingsSave_ = true;
}

void ShellWindow::saveSettingsNow() {
    saveWindowPlacement();
    hasPendingSettingsSave_ = false;
    AppSettingsStore::save(settingsPath_, settings_);
}

DWORD WINAPI ShellWindow::profileSaveWorkerProc(LPVOID param) {
    ProfileSaveJob* job = static_cast<ProfileSaveJob*>(param);
    if (job) {
        if (ProfileStore::save(job->path, job->profile)) {
            ProfileStore::saveLastActive(job->profilesDir, job->path);
        }
        delete job;
    }
    return 0;
}

void ShellWindow::applyProfileToEngine(bool autoSaveNow) {
    engine_->setProfile(profile_);
    hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
    profileDirty_ = true;
    lastHudSig_.valid = false;
    lastTopBarSig_.valid = false;
    if (autoSaveNow) {
        saveCurrentProfile(true);
    }
    hudOverlay_.invalidate();
    invalidateShell();
}

void ShellWindow::applyTuningToEngine() {
    // N28: non-disruptive live update (preserves active touches) + 500ms debounced async disk save!
    engine_->updateTuning(profile_);
    hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
    profileDirty_ = true;
    lastHudSig_.valid = false;
    lastTopBarSig_.valid = false;
    profileSaveDueTick_ = GetTickCount() + 500;
    hasPendingProfileSave_ = true;
    hudOverlay_.invalidate();
    invalidateShell();
}

bool ShellWindow::saveCurrentProfile(bool silent) {
    if (profilePath_.empty()) return false;
    if (ProfileStore::save(profilePath_, profile_)) {
        ProfileStore::saveLastActive(profilesDir_, profilePath_);
        profileDirty_ = false;
        hasPendingProfileSave_ = false;
        if (!silent) {
            drawerPanel_.refreshProfileList();
            showToast("Saved config -> " + profile_.name, 2000);
        }
        return true;
    }
    if (!silent) {
        showToast("ERROR: Could not write " + profilePath_, 3000);
    }
    return false;
}

void ShellWindow::flushOnExit() {
    if (!profilePath_.empty()) {
        if (profileDirty_) saveCurrentProfile(true);
        else ProfileStore::saveLastActive(profilesDir_, profilePath_);
    }
    saveSettingsNow();
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
        const int tbH = metrics_.topBarH();
        const int rW  = metrics_.railW();
        topBarRc_ = { 0, 0, w, tbH };
        railRc_   = { w - rW, tbH, w, h };
        int rightEdge = railRc_.left;
        if (drawerOpen_) {
            int dw = metrics_.drawerW();
            int minVp = metrics_.dp(360);
            int minDw = metrics_.dp(260);
            if (rightEdge - dw < minVp) dw = rightEdge - minVp;
            if (dw < minDw) dw = minDw;
            drawerRc_ = { rightEdge - dw, tbH, rightEdge, h };
            rightEdge = drawerRc_.left;
        } else {
            drawerRc_ = { 0, 0, 0, 0 };
        }
        viewportRc_ = { 0, tbH, rightEdge, h };
    }

    viewportHost_.setBounds(viewportRc_);
    if (bridge_->isVirtualDisplayActive() && settings_.virtualDisplaySpec != "auto" &&
        (viewportRc_.right - viewportRc_.left) >= 160 && (viewportRc_.bottom - viewportRc_.top) >= 160) {
        pendingVdResizeTick_ = GetTickCount() + 150;
        hasPendingVdResize_ = true;
    }
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
            if (pw >= 64 && ph >= 64 &&
                (settings_.lastPhoneW != static_cast<int>(pw) || settings_.lastPhoneH != static_cast<int>(ph))) {
                settings_.lastPhoneW = static_cast<int>(pw);
                settings_.lastPhoneH = static_cast<int>(ph);
                scheduleSettingsSave();
            }
            viewportHost_.updateVideoLayout(pw, ph);
            viewportHost_.renderNativeFrame(true);
            invalidateShell();
        }
        if (!nativeVideoSession_ && !noMirror_ && !viewportHost_.isMirrorEmbedded() && bridge_->isMirrorRunning()) {
            if (viewportHost_.tryEmbedMirror(mirrorTitle_, bridge_->mirrorProcessId(), pw, ph)) {
                sawEmbeddedMirror_ = true;
                layoutChildren();
                showToast("Connected! Click KEYS (F2) to edit outside the phone | " + KeyCodes::toName(profile_.toggleAimVk) + " / F1 for Aim", 3800);
            }
        }
    } else if (demoMode_) {
        uint32_t dw = static_cast<uint32_t>(settings_.lastPhoneW >= 64 ? settings_.lastPhoneW : 1920);
        uint32_t dh = static_cast<uint32_t>(settings_.lastPhoneH >= 64 ? settings_.lastPhoneH : 1080);
        lastPhoneW_ = dw;
        lastPhoneH_ = dh;
        viewportHost_.updateVideoLayout(dw, dh);
    }

    if (viewportHost_.hwnd() && IsWindow(viewportHost_.hwnd())) {
        if (showOnboarding_) {
            if (IsWindowVisible(viewportHost_.hwnd())) ShowWindow(viewportHost_.hwnd(), SW_HIDE);
        } else if (viewportHost_.isNativeVideoActive() || viewportHost_.isMirrorEmbedded()) {
            if (!IsWindowVisible(viewportHost_.hwnd())) ShowWindow(viewportHost_.hwnd(), SW_SHOWNA);
        }
    }

    bool showHud = !minimized && !showOnboarding_ && (connected || demoMode_);
    RECT vidScr = viewportHost_.activeVideoScreenRect();
    hudOverlay_.syncBounds(vidScr, showHud);
    if (aimMode_ && showHud) {
        int cx = (vidScr.left + vidScr.right) / 2;
        int cy = (vidScr.top + vidScr.bottom) / 2;
        RECT curClip;
        if (!GetClipCursor(&curClip) ||
            curClip.left != cx || curClip.top != cy ||
            curClip.right != cx + 1 || curClip.bottom != cy + 1) {
            updateClipCursor();
        }
    }
    syncInputHookSnapshot();
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
    if (!toolsReady_ || InterlockedCompareExchange(&refreshingDevices_, 1, 0) != 0) return;
    asyncCtx_->addRef();
    HANDLE hRef = CreateThread(NULL, 0, refreshDevicesThreadProc,
                               new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
    if (hRef) CloseHandle(hRef);
    else {
        InterlockedExchange(&refreshingDevices_, 0);
        asyncCtx_->release();
    }
}

DWORD WINAPI ShellWindow::startupToolsThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (job && job->ctx) {
        ShellWindow* self = job->shell;
        ToolPaths tools;
        std::string err;
        bool ok = self->bridge_->locateTools(tools, err);
        std::vector<AdbDeviceInfo> devs;
        if (ok) {
            self->bridge_->listDevices(tools, devs);
        }
        EnterCriticalSection(&self->auxLock_);
        self->pendingTools_ = tools;
        self->pendingToolsErr_ = err;
        self->pendingToolsOk_ = ok;
        self->pendingAdbDevices_ = devs;
        LeaveCriticalSection(&self->auxLock_);
        job->ctx->postMsg(MSG_TOOLS_READY, 0, 0);
        job->ctx->release();
    }
    delete job;
    return 0;
}

DWORD WINAPI ShellWindow::refreshDevicesThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (job && job->ctx) {
        ShellWindow* self = job->shell;
        std::vector<AdbDeviceInfo> devs;
        bool ok = self->bridge_->listDevices(self->tools_, devs);
        EnterCriticalSection(&self->auxLock_);
        self->pendingAdbDevices_ = devs;
        self->pendingDevicesRefreshOk_ = ok;
        LeaveCriticalSection(&self->auxLock_);
        InterlockedExchange(&self->refreshingDevices_, 0);
        job->ctx->postMsg(MSG_DEVICES_REFRESHED, 0, 0);
        job->ctx->release();
    }
    delete job;
    return 0;
}

DWORD WINAPI ShellWindow::deviceMonitorThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (!job || !job->ctx) {
        delete job;
        return 0;
    }
    ShellWindow* self = job->shell;
    std::vector<AdbDeviceInfo> lastDevs;
    bool haveSnapshot = false;

    while (!self->stopAuxThreads_ && job->ctx->alive) {
        DWORD waitRes = WAIT_TIMEOUT;
        if (self->deviceMonitorWakeEvent_) {
            waitRes = WaitForSingleObject(self->deviceMonitorWakeEvent_, 2000);
        } else {
            Sleep(2000);
        }
        if (self->stopAuxThreads_ || !job->ctx->alive) break;

        if (!self->toolsReady_ || self->bridge_->isConnected() || self->connecting_ ||
            self->demoMode_ || self->repairingTools_) {
            continue;
        }
        if (InterlockedCompareExchange(&self->refreshingDevices_, 1, 0) != 0) {
            continue;
        }

        std::vector<AdbDeviceInfo> devs;
        bool ok = self->bridge_->listDevices(self->tools_, devs);
        bool changed = !haveSnapshot || (devs.size() != lastDevs.size());
        if (!changed) {
            for (size_t i = 0; i < devs.size(); ++i) {
                if (devs[i].serial != lastDevs[i].serial ||
                    devs[i].state != lastDevs[i].state ||
                    devs[i].model != lastDevs[i].model) {
                    changed = true;
                    break;
                }
            }
        }
        if (changed || waitRes == WAIT_OBJECT_0 || self->reconnecting_) {
            lastDevs = devs;
            haveSnapshot = true;
            EnterCriticalSection(&self->auxLock_);
            self->pendingAdbDevices_ = devs;
            self->pendingDevicesRefreshOk_ = ok;
            LeaveCriticalSection(&self->auxLock_);
            job->ctx->postMsg(MSG_DEVICES_REFRESHED, 0, 0);
        }
        InterlockedExchange(&self->refreshingDevices_, 0);
    }

    job->ctx->release();
    delete job;
    return 0;
}

DWORD WINAPI ShellWindow::repairToolsThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (job && job->ctx) {
        ShellWindow* self = job->shell;
        std::string scriptPath = self->exeDir_ + "\\setup_scrcpy.ps1";
        if (GetFileAttributesA(scriptPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string cmd = "powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath + "\"";
            std::wstring wcmd(cmd.begin(), cmd.end());
            std::vector<wchar_t> buf(wcmd.begin(), wcmd.end());
            buf.push_back(L'\0');
            STARTUPINFOW si;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi;
            ZeroMemory(&pi, sizeof(pi));
            if (CreateProcessW(NULL, buf.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 120000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
        }

        ToolPaths tools;
        std::string err;
        bool ok = self->bridge_->locateTools(tools, err);
        std::vector<AdbDeviceInfo> devs;
        if (ok) {
            self->bridge_->listDevices(tools, devs);
        }
        EnterCriticalSection(&self->auxLock_);
        self->pendingTools_ = tools;
        self->pendingToolsErr_ = err;
        self->pendingToolsOk_ = ok;
        self->pendingAdbDevices_ = devs;
        LeaveCriticalSection(&self->auxLock_);
        InterlockedExchange(&self->repairingTools_, 0);
        job->ctx->postMsg(MSG_REPAIR_TOOLS_DONE, 0, 0);
        job->ctx->release();
    }
    delete job;
    return 0;
}

void ShellWindow::onConnectStep(void* ctx, int step) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self) return;
    InterlockedExchange(&self->connectStep_, step);
    if (self->asyncCtx_) {
        self->asyncCtx_->postMsg(MSG_CONNECT_STEP, static_cast<WPARAM>(step), 0);
    }
}

void ShellWindow::startAsyncRepairTools() {
    if (InterlockedCompareExchange(&repairingTools_, 1, 0) != 0) return;
    hubStatusMsg_ = "Running phone tools setup (setup_scrcpy.ps1)...";
    asyncCtx_->addRef();
    HANDLE hRep = CreateThread(NULL, 0, repairToolsThreadProc,
                               new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
    if (hRep) CloseHandle(hRep);
    else {
        InterlockedExchange(&repairingTools_, 0);
        asyncCtx_->release();
    }
    invalidateShell();
}

void ShellWindow::applyOnboardingProfileChoice(OnboardingProfileChoice choice) {
    if (choice == OB_PROF_CONTROLLER) {
        settings_.gamepadEnabled = true;
        engine_->setGamepadEnabled(true);
        scheduleSettingsSave();
        std::string deskPath = ensureDesktopProfileExists();
        switchProfilePath(deskPath, false);
        showToast("Starter set: Controller + Desktop Typing", 2200);
    } else if (choice == OB_PROF_GAMING) {
        std::string starterPath = profilesDir_ + "\\freefire.cfg";
        if (GetFileAttributesA(starterPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Profile p = ProfileDefaults::createFreeFire();
            p.name = "Game Keymap Starter";
            ProfileStore::save(starterPath, p);
        }
        switchProfilePath(starterPath, false);
        showToast("Starter set: Game Keymap (" + profile_.name + ")", 2200);
    } else {
        std::string deskPath = ensureDesktopProfileExists();
        switchProfilePath(deskPath, false);
        showToast("Starter set: Desktop (Typing)", 2200);
    }
}

void ShellWindow::finishOnboarding() {
    showOnboarding_ = false;
    onboardingView_.clearFocus();
    settings_.onboardingDone = true;
    saveSettingsNow();
    layoutChildren();
    syncInputHookSnapshot();
    invalidateShell();
}

void ShellWindow::buildHubViewState(HubViewState& out) const {
    out = HubViewState();
    int readyCnt = 0;
    int unauthCnt = 0;
    for (size_t i = 0; i < adbDevices_.size(); ++i) {
        if (adbDevices_[i].state == "device") ++readyCnt;
        else if (adbDevices_[i].state == "unauthorized") ++unauthCnt;
    }

    ConnStateInput ci;
    ci.toolsReady = toolsReady_;
    ci.streaming = bridge_->isConnected();
    ci.connecting = (connecting_ != 0);
    ci.reconnecting = reconnecting_;
    ci.reconnectCancelled = reconnectCancelled_;
    ci.reconnectAttempt = reconnectAttempt_;
    ci.maxReconnectAttempts = 5;
    ci.hasError = !lastConnectError_.empty();
    ci.readyDeviceCount = readyCnt;
    ci.unauthorizedDeviceCount = unauthCnt;

    out.connState = ConnectionStateMachine::evaluate(ci);
    int cs = static_cast<int>(connectStep_);
    if (cs < 0) cs = 0;
    if (cs > 2) cs = 2;
    out.connectStep = static_cast<ConnectProgressStep>(cs);

    DWORD nowTick = GetTickCount();
    int remMs = (nowTick < reconnectNextTick_) ? static_cast<int>(reconnectNextTick_ - nowTick) : 0;
    out.reconnectCountdownSec = (remMs + 999) / 1000;
    if (out.reconnectCountdownSec < 1 && out.connState == CONN_RECONNECTING) out.reconnectCountdownSec = 1;
    out.reconnectAttempt = reconnectAttempt_;
    out.maxReconnectAttempts = 5;
    out.statusMsg = !lastConnectError_.empty() ? lastConnectError_ : hubStatusMsg_;
    out.errorDetails = lastConnectError_;
    out.secureAppBlocked = secureAppDetected_;
    out.devices = &adbDevices_;
    out.wirelessAddr = settings_.lastWirelessAddr;
    out.editingWirelessAddr = editingHubWireless_;
    out.showingQrPair = showingQrPair_;
    out.qrMatrix = &qrMatrix_;
    EnterCriticalSection(&auxLock_);
    out.qrStatus = qrPairStatus_;
    LeaveCriticalSection(&auxLock_);
}

void ShellWindow::onVideoPacket(void* ctx, const uint8_t* data, size_t size,
                                uint64_t ptsUs, bool isConfig, bool isKeyFrame,
                                bool present, int64_t qpcPktRecv) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self || !self->nativeVideoSession_ || !data || size == 0) return;

    if (InterlockedCompareExchange(&self->reinitDecoderFlag_, 0, 1) == 1) {
        self->h264Decoder_.shutdown();
    }

    if (!self->h264Decoder_.isInitialized()) {
        uint32_t pw = 1920, ph = 1080;
        self->bridge_->getScreenSize(pw, ph);
        ID3D11Device* gpuDev = (self->viewportHost_.supportsGpuNv12() && !self->viewportHost_.isNativeDeviceLost())
            ? self->viewportHost_.d3dDevice()
            : nullptr;
        std::string err;
        bool inited = self->h264Decoder_.init(pw, ph, err, gpuDev);
        if (!inited && gpuDev != nullptr) {
            // N3: Hardware decoder init failed -> automatically fall back to CPU H.264 MFT before giving up!
            self->bridge_->appendLog("[Video] DXVA H.264 init failed (" + err + "), retrying CPU decoder...");
            inited = self->h264Decoder_.init(pw, ph, err, nullptr);
        }
        if (!inited) {
            self->bridge_->appendLog("[Video] H.264 MFT init failed: " + err);
            self->nativeVideoSession_ = false;
            if (self->hwnd_ && InterlockedCompareExchange(&self->fallbackPending_, 1, 0) == 0) {
                PostMessageW(self->hwnd_, MSG_VIDEO_FALLBACK, 0, 0);
            }
            return;
        }
        self->h264Decoder_.setGpuFrameCallback(ShellWindow::onDecodedGpuFrame, self);
        self->h264Decoder_.setCpuViewCallback(ShellWindow::onDecodedCpuView, self);
        self->bridge_->appendLog(self->h264Decoder_.isHardwareAccelerated()
            ? "[Video] DXVA GPU hardware H.264 decoder active (zero-copy VRAM -> D3D11)"
            : "[Video] Software H.264 decoder active (direct upload -> D3D11)");
    }
    self->h264Decoder_.decodePacket(data, size, ptsUs, isConfig, isKeyFrame, present, qpcPktRecv);
}

void ShellWindow::onDecodedGpuFrame(void* ctx, ID3D11Texture2D* nv12Tex, uint32_t subresourceIdx,
                                    uint32_t cropX, uint32_t cropY, uint32_t outW, uint32_t outH,
                                    const FrameTiming& timing) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self || !self->nativeVideoSession_) return;
    if (!timing.present) {
        self->telemetry_.recordSkipped(timing);
        return;
    }
    FrameTiming ft = timing;
    // Present directly from video thread so the Win32 UI / input-hook thread stays <1% CPU!
    if (self->viewportHost_.presentGpuNV12Frame(nv12Tex, subresourceIdx, cropX, cropY, outW, outH, true, &ft)) {
        self->telemetry_.recordPresented(ft);
    }
    if (self->viewportHost_.isNativeDeviceLost() && self->hwnd_ &&
        InterlockedCompareExchange(&self->deviceResetPending_, 1, 0) == 0) {
        PostMessageW(self->hwnd_, MSG_DEVICE_LOST, 0, 0);
    }
}

void ShellWindow::onDecodedCpuView(void* ctx, const uint8_t* yPlane, const uint8_t* uvPlane,
                                   uint32_t stride, uint32_t cropX, uint32_t cropY,
                                   uint32_t outW, uint32_t outH, const FrameTiming& timing) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self || !self->nativeVideoSession_) return;
    if (!timing.present) {
        self->telemetry_.recordSkipped(timing);
        return;
    }
    FrameTiming ft = timing;
    // Direct upload + Present on video thread (0 intermediate copies, 0 UI-thread stall!)
    self->viewportHost_.presentCpuNV12Frame(yPlane, uvPlane, stride, cropX, cropY, outW, outH, true, &ft);
    self->telemetry_.recordPresented(ft);
    if (self->viewportHost_.isNativeDeviceLost() && self->hwnd_ &&
        InterlockedCompareExchange(&self->deviceResetPending_, 1, 0) == 0) {
        PostMessageW(self->hwnd_, MSG_DEVICE_LOST, 0, 0);
    }
}

void ShellWindow::onAudioPacket(void* ctx, AudioCodecId codec,
                                const uint8_t* data, size_t size,
                                uint64_t ptsUs, bool isConfig) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self || !self->nativeVideoSession_ || self->audioPlayer_.isStopRequested() || !data || size == 0) {
        return;
    }
    if (!self->audioPlayer_.isRunning()) {
        const int bufMs = (self->forceAudioBufferMs_ >= 15 && self->forceAudioBufferMs_ <= 100)
            ? self->forceAudioBufferMs_
            : self->settings_.audioBufferMs;
        std::string aErr;
        if (self->audioPlayer_.start(codec, bufMs, aErr)) {
            char abuf[96];
            std::snprintf(abuf, sizeof(abuf), "[Audio] WASAPI playback started (%s, %dms buffer)",
                          (codec == AUDIO_CODEC_RAW) ? "raw PCM" : "AAC", bufMs);
            self->bridge_->appendLog(abuf);
        } else if (!aErr.empty()) {
            self->bridge_->appendLog("[Audio] WASAPI start failed: " + aErr);
        }
    }
    self->audioPlayer_.onAudioPacket(codec, data, size, ptsUs, isConfig);
}

void ShellWindow::onHudBitmapReady(void* ctx, const void* bgraPremulPixels, int w, int h) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self || !self->viewportHost_.isNativeVideoActive()) return;
    self->viewportHost_.updateEmbeddedHud(bgraPremulPixels, w, h);
    // In Play Mode with live video streaming, the video thread composites this updated HUD on
    // the very next frame (<16ms). Only force an immediate Present in Edit Mode or when stream is idle.
    DWORD sincePresent = GetTickCount() - self->viewportHost_.lastPresentTick();
    if (self->editMode_ || sincePresent >= 45) {
        self->viewportHost_.renderNativeFrame(true);
    }
}

bool ShellWindow::mapShellClientToVideoLocal(int mx, int my, int& outVx, int& outVy, bool requireInside) const {
    ViewportRect vr = viewportHost_.activeVideoClientRect();
    int left = viewportRc_.left + vr.x;
    int top  = viewportRc_.top  + vr.y;
    outVx = mx - left;
    outVy = my - top;
    if (!requireInside) return (vr.w > 0 && vr.h > 0);
    return (outVx >= 0 && outVx < vr.w && outVy >= 0 && outVy < vr.h);
}

bool ShellWindow::mapShellClientToVideoNorm(int mx, int my, float& outNx, float& outNy, bool requireInside) const {
    ViewportRect vr = viewportHost_.activeVideoClientRect();
    if (vr.w <= 0 || vr.h <= 0) return false;
    int vx = mx - (viewportRc_.left + vr.x);
    int vy = my - (viewportRc_.top  + vr.y);
    if (requireInside && (vx < 0 || vx >= vr.w || vy < 0 || vy >= vr.h)) {
        return false;
    }
    float nx = static_cast<float>(vx) / static_cast<float>(vr.w);
    float ny = static_cast<float>(vy) / static_cast<float>(vr.h);
    if (nx < 0.0f) nx = 0.0f;
    if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f;
    if (ny > 1.0f) ny = 1.0f;
    outNx = nx;
    outNy = ny;
    return true;
}

DWORD WINAPI ShellWindow::connectThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    ShellWindow* self = job->shell;
    std::string err;
    bool ok = false;

    // S1: always tear down any previous session off the UI thread!
    self->bridge_->setVideoPacketCallback(nullptr, nullptr);
    self->bridge_->setAudioPacketCallback(nullptr, nullptr);
    self->audioPlayer_.requestStop();
    self->bridge_->disconnect();
    self->audioPlayer_.stop();
    self->telemetry_.reset();

    if (self->settings_.useNativeVideo && !self->noMirror_) {
        std::string d3dInitErr;
        if (!self->viewportHost_.ensureNativeDeviceInitialized(d3dInitErr, self->settings_.vsyncEnabled)) {
            self->bridge_->appendLog("[D3D11] Pre-init skipped: " + d3dInitErr);
        }
        // Stage 2 + F7 Single-Connection Mode: ONE scrcpy-server delivers video + audio + control!
        const bool wantAudio = (self->settings_.mirrorAudio || self->forceAudioOn_);
        self->nativeVideoSession_ = true;
        self->bridge_->setVideoPacketCallback(ShellWindow::onVideoPacket, self);
        if (wantAudio) {
            self->audioPlayer_.prepareForSession();
            self->bridge_->setAudioPacketCallback(ShellWindow::onAudioPacket, self);
        }

        std::vector<VideoEncoderInfo> probedEncs;
        std::string chosenEnc = self->settings_.encoderForDevice(job->arg);
        self->bridge_->listH264Encoders(self->tools_, job->arg, probedEncs);
        std::string discoveredHw = ScrcpyBridge::pickBestHardwareH264Encoder(probedEncs);
        if (chosenEnc.empty()) {
            chosenEnc = discoveredHw;
        }
        EnterCriticalSection(&self->auxLock_);
        self->deviceEncoders_ = probedEncs;
        self->activeEncoderName_ = chosenEnc;
        self->pendingDiscoveredSerial_.clear();
        self->pendingDiscoveredEncoder_.clear();
        LeaveCriticalSection(&self->auxLock_);

        BridgeStreamConfig cfg;
        cfg.maxSize = self->settings_.mirrorMaxSize;
        cfg.maxFps = self->settings_.mirrorFps;
        cfg.bitrateMbps = self->settings_.mirrorBitrateMbps;
        cfg.stayAwake = self->settings_.stayAwake;
        cfg.clipboardAutosync = self->settings_.clipboardSync;
        cfg.enableAudio = wantAudio;
        cfg.audioCodec = self->forceAudioCodec_;
        cfg.virtualDisplay = self->settings_.virtualDisplayMode;
        cfg.virtualDisplaySpec = self->settings_.virtualDisplaySpec;
        cfg.videoCodecOptions = self->settings_.videoCodecOptions;
        cfg.videoEncoder = chosenEnc;
        self->clipGuard_.reset();

        ok = self->bridge_->connectSession(self->tools_, cfg, err, job->arg);
        if (ok) {
            std::string actSer = self->bridge_->activeSerial();
            if (!discoveredHw.empty() && !actSer.empty() && self->settings_.encoderForDevice(actSer).empty()) {
                EnterCriticalSection(&self->auxLock_);
                self->pendingDiscoveredSerial_ = actSer;
                self->pendingDiscoveredEncoder_ = discoveredHw;
                LeaveCriticalSection(&self->auxLock_);
            }
            PhoneThermalInfo ti;
            if (self->bridge_->queryThermalTelemetry(ti)) {
                EnterCriticalSection(&self->auxLock_);
                self->thermalInfo_ = ti;
                LeaveCriticalSection(&self->auxLock_);
            }
            if (cfg.virtualDisplay) {
                std::string appToLaunch = "com.android.settings";
                if (self->profile_.keymapEnabled && !self->profile_.appPackage.empty()) {
                    size_t comma = self->profile_.appPackage.find(',');
                    std::string first = (comma == std::string::npos)
                        ? self->profile_.appPackage
                        : self->profile_.appPackage.substr(0, comma);
                    if (!first.empty()) appToLaunch = first;
                }
                self->bridge_->sendStartApp(appToLaunch);
            }
            if (self->settings_.turnScreenOff && !cfg.virtualDisplay) {
                self->bridge_->setDisplayPower(false);
            }
        } else {
            self->nativeVideoSession_ = false;
            self->bridge_->setVideoPacketCallback(nullptr, nullptr);
            self->bridge_->setAudioPacketCallback(nullptr, nullptr);
            self->audioPlayer_.stop();
        }
    } else {
        self->nativeVideoSession_ = false;
        self->bridge_->setVideoPacketCallback(nullptr, nullptr);
        self->bridge_->setAudioPacketCallback(nullptr, nullptr);
        ok = self->bridge_->connectControl(self->tools_, self->profile_.controlMaxSize, err,
                                           job->arg, self->settings_.stayAwake);
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
    }

    EnterCriticalSection(&self->auxLock_);
    self->asyncResultOk_ = ok;
    self->asyncResultMsg_ = ok ? ("Connected to " + self->bridge_->deviceName()) : err;
    LeaveCriticalSection(&self->auxLock_);
    InterlockedExchange(&self->connecting_, 0);
    job->ctx->postMsg(MSG_CONNECT_DONE, 0, 0);
    job->ctx->release();
    delete job;
    return 0;
}

DWORD WINAPI ShellWindow::disconnectThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    ShellWindow* self = job->shell;
    self->bridge_->setAudioPacketCallback(nullptr, nullptr);
    self->audioPlayer_.requestStop();
    self->bridge_->disconnect();
    self->audioPlayer_.stop();
    std::vector<AdbDeviceInfo> devs;
    if (job->intArg != 0 && self->toolsReady_) {
        self->bridge_->listDevices(self->tools_, devs);
    }
    EnterCriticalSection(&self->auxLock_);
    self->pendingAdbDevices_ = devs;
    LeaveCriticalSection(&self->auxLock_);
    job->ctx->postMsg(MSG_DISCONNECT_DONE, static_cast<WPARAM>(job->intArg), 0);
    job->ctx->release();
    delete job;
    return 0;
}

DWORD WINAPI ShellWindow::legacyFallbackThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    ShellWindow* self = job->shell;
    std::string serial = self->bridge_->activeSerial();
    self->bridge_->setVideoPacketCallback(nullptr, nullptr);
    self->bridge_->setAudioPacketCallback(nullptr, nullptr);
    self->audioPlayer_.requestStop();
    self->bridge_->disconnect();
    self->audioPlayer_.stop();

    self->nativeVideoSession_ = false;
    std::string err;
    bool ok = self->bridge_->connectControl(self->tools_, self->profile_.controlMaxSize, err,
                                            serial, self->settings_.stayAwake);
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
    EnterCriticalSection(&self->auxLock_);
    self->asyncResultOk_ = ok;
    self->asyncResultMsg_ = ok ? self->pendingFallbackReason_ : err;
    LeaveCriticalSection(&self->auxLock_);
    InterlockedExchange(&self->connecting_, 0);
    InterlockedExchange(&self->fallbackPending_, 0);
    job->ctx->postMsg(MSG_CONNECT_DONE, 0, 0);
    job->ctx->release();
    delete job;
    return 0;
}

void ShellWindow::startAsyncLegacyFallback(const std::string& reason) {
    if (InterlockedCompareExchange(&connecting_, 1, 0) != 0) return;
    setAimMode(false);
    hudOverlay_.setEmbeddedD3DMode(false);
    bridge_->setVideoPacketCallback(nullptr, nullptr);
    bridge_->setAudioPacketCallback(nullptr, nullptr);
    audioPlayer_.requestStop();
    h264Decoder_.shutdown();
    viewportHost_.disableNativeVideo();
    nativeVideoSession_ = false;
    pendingFallbackReason_ = reason;
    hubStatusMsg_ = reason + " Switching to scrcpy fallback...";
    asyncCtx_->addRef();
    HANDLE hTh = CreateThread(NULL, 0, legacyFallbackThreadProc,
                              new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
    if (hTh) CloseHandle(hTh);
    else {
        InterlockedExchange(&connecting_, 0);
        asyncCtx_->release();
    }
    syncInputHookSnapshot();
    invalidateShell();
}

void ShellWindow::startAsyncConnect(const std::string& serial) {
    if (!toolsReady_ || InterlockedCompareExchange(&connecting_, 1, 0) != 0) return;
    stopQrWirelessPairing();
    hudOverlay_.cancelRebind();
    drawerPanel_.clearFocus();
    setAimMode(false);
    hudOverlay_.setEmbeddedD3DMode(false);
    bridge_->setVideoPacketCallback(nullptr, nullptr);
    bridge_->setAudioPacketCallback(nullptr, nullptr);
    audioPlayer_.requestStop();
    h264Decoder_.shutdown();
    viewportHost_.detachMirror();
    demoMode_ = false;
    sawEmbeddedMirror_ = false;
    nativeVideoSession_ = false;
    userDisconnected_ = false;
    lastConnectError_.clear();
    InterlockedExchange(&connectStep_, 0);
    pendingSerial_ = serial;

    hubStatusMsg_ = settings_.virtualDisplayMode
        ? "Launching Second-Phone Virtual Display stream..."
        : "Connecting single-stream D3D11 phone viewport...";
    asyncCtx_->addRef();
    HANDLE hConn = CreateThread(NULL, 0, connectThreadProc,
                                new SimpleAsyncJob{ this, asyncCtx_, serial, 0 }, 0, NULL);
    if (hConn) CloseHandle(hConn);
    else {
        InterlockedExchange(&connecting_, 0);
        asyncCtx_->release();
    }
    syncInputHookSnapshot();
    invalidateShell();
}

DWORD WINAPI ShellWindow::wirelessThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    ShellWindow* self = job->shell;
    std::string msg;
    bool ok = self->bridge_->connectWireless(self->tools_, job->arg, msg);
    EnterCriticalSection(&self->auxLock_);
    self->asyncResultOk_ = ok;
    self->asyncResultMsg_ = msg;
    LeaveCriticalSection(&self->auxLock_);
    InterlockedExchange(&self->connecting_, 0);
    job->ctx->postMsg(MSG_WIRELESS_DONE, 0, 0);
    job->ctx->release();
    delete job;
    return 0;
}

void ShellWindow::startAsyncWirelessConnect(const std::string& ipPort) {
    if (!toolsReady_ || ipPort.empty() || InterlockedCompareExchange(&connecting_, 1, 0) != 0) return;
    stopQrWirelessPairing();
    settings_.lastWirelessAddr = ipPort;
    scheduleSettingsSave();
    pendingWirelessIp_ = ipPort;
    hubStatusMsg_ = "Connecting Wireless ADB to " + ipPort + "...";
    asyncCtx_->addRef();
    HANDLE hW = CreateThread(NULL, 0, wirelessThreadProc,
                             new SimpleAsyncJob{ this, asyncCtx_, ipPort, 0 }, 0, NULL);
    if (hW) CloseHandle(hW);
    else {
        InterlockedExchange(&connecting_, 0);
        asyncCtx_->release();
    }
    invalidateShell();
}

DWORD WINAPI ShellWindow::qrPairThreadProc(LPVOID param) {
    QrPairJob* job = static_cast<QrPairJob*>(param);
    auto setStatus = [&](const std::string& st, const std::string& autoAddr = "") {
        if (job->cancelled || !job->ctx || !job->ctx->alive) return;
        EnterCriticalSection(job->auxLock);
        *job->statusTarget = st;
        if (!autoAddr.empty()) *job->autoConnectTarget = autoAddr;
        LeaveCriticalSection(job->auxLock);
        job->ctx->postMsg(MSG_QR_PAIR_UPDATE, 0, 0);
    };

    bool paired = false;
    while (!job->cancelled && job->ctx && job->ctx->alive) {
        std::vector<MdnsServiceInfo> svcs;
        if (job->bridge->listMdnsServices(job->tools, svcs)) {
            if (!paired && !job->cancelled) {
                for (size_t i = 0; i < svcs.size(); ++i) {
                    if (svcs[i].type.find("_adb-tls-pairing") != std::string::npos &&
                        (svcs[i].name == job->serviceName || svcs[i].name.find(job->serviceName) != std::string::npos)) {
                        setStatus("Phone scanned QR! Pairing " + svcs[i].addr + "...");
                        std::string pairMsg;
                        if (job->bridge->pairWireless(job->tools, svcs[i].addr, job->password, pairMsg)) {
                            paired = true;
                            setStatus("Paired! Looking for Wireless ADB connect port...");
                        } else {
                            setStatus("Pair attempt: " + pairMsg);
                        }
                        break;
                    }
                }
            }
            if (paired && !job->cancelled) {
                for (size_t i = 0; i < svcs.size(); ++i) {
                    if (svcs[i].type.find("_adb-tls-connect") != std::string::npos) {
                        setStatus("Connecting wirelessly to " + svcs[i].addr + "...", svcs[i].addr);
                        break;
                    }
                }
                if (!svcs.empty()) break;
            }
        }
        for (int i = 0; i < 10 && !job->cancelled && job->ctx && job->ctx->alive; ++i) {
            Sleep(100);
        }
    }

    if (job->ctx) job->ctx->release();
    delete job;
    return 0;
}

void ShellWindow::startQrWirelessPairing() {
    if (!toolsReady_) {
        hubStatusMsg_ = "ADB tools not ready yet.";
        invalidateShell();
        return;
    }
    stopQrWirelessPairing();

    // B6: cryptographically random 12-char pairing code & service suffix via BCryptGenRandom
    qrServiceName_ = "studio-PT" + QrCodeGen::generateSecurePairingCode(6);
    qrPairPassword_ = QrCodeGen::generateSecurePairingCode(12);

    std::string payload = QrCodeGen::buildAndroidAdbPairPayload(qrServiceName_, qrPairPassword_);
    QrCodeGen::encode(payload, qrMatrix_);

    EnterCriticalSection(&auxLock_);
    qrPairStatus_ = "Scan QR from Wireless Debugging (Code: " + qrPairPassword_ + ")";
    qrAutoConnectAddr_.clear();
    LeaveCriticalSection(&auxLock_);

    showingQrPair_ = true;
    QrPairJob* job = new QrPairJob();
    asyncCtx_->addRef();
    job->ctx = asyncCtx_;
    job->bridge = bridge_;
    job->tools = tools_;
    job->serviceName = qrServiceName_;
    job->password = qrPairPassword_;
    job->statusTarget = &qrPairStatus_;
    job->autoConnectTarget = &qrAutoConnectAddr_;
    job->auxLock = &auxLock_;
    job->cancelled = 0;
    activeQrJob_ = job;

    HANDLE hQr = CreateThread(NULL, 0, qrPairThreadProc, job, 0, NULL);
    if (hQr) CloseHandle(hQr);
    else {
        activeQrJob_ = nullptr;
        asyncCtx_->release();
        delete job;
    }
    invalidateShell();
}

void ShellWindow::stopQrWirelessPairing() {
    // S1 / E9: cancel asynchronously with ZERO WaitForSingleObject on UI thread!
    if (activeQrJob_) {
        InterlockedExchange(&activeQrJob_->cancelled, 1);
        activeQrJob_ = nullptr;
    }
    showingQrPair_ = false;
}

void ShellWindow::disconnectDevice(bool returnToHub, bool userInitiated) {
    InterlockedExchange(&stopProbe_, 1);
    hudOverlay_.cancelRebind();
    drawerPanel_.clearFocus();
    setAimMode(false);
    engine_->releaseAllTouches();
    hudOverlay_.setEmbeddedD3DMode(false);
    bridge_->setVideoPacketCallback(nullptr, nullptr);
    bridge_->setAudioPacketCallback(nullptr, nullptr);
    audioPlayer_.requestStop();
    h264Decoder_.shutdown();
    viewportHost_.detachMirror();
    sawEmbeddedMirror_ = false;
    nativeVideoSession_ = false;
    d3dHudDragging_ = false;
    secureAppDetected_ = false;
    userDisconnected_ = userInitiated;
    if (userInitiated) {
        reconnecting_ = false;
        reconnectCancelled_ = false;
        reconnectAttempt_ = 0;
        lastConnectError_.clear();
    }
    if (returnToHub) {
        demoMode_ = false;
        setEditMode(false);
        hubStatusMsg_ = userInitiated ? "Disconnected." : "Connection interrupted.";
    }
    layoutChildren();
    syncInputHookSnapshot();
    invalidateShell();

    // S1: run adb forward cleanup / device refresh off UI thread!
    asyncCtx_->addRef();
    HANDLE hDis = CreateThread(NULL, 0, disconnectThreadProc,
                               new SimpleAsyncJob{ this, asyncCtx_, "", returnToHub ? 1 : 0 }, 0, NULL);
    if (hDis) CloseHandle(hDis);
    else asyncCtx_->release();
}

DWORD WINAPI ShellWindow::screenshotThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (job && job->ctx) {
        ShellWindow* self = job->shell;
        std::string fullPath = job->arg;
        size_t sl = fullPath.find_last_of("\\/");
        std::string fname = (sl == std::string::npos) ? fullPath : fullPath.substr(sl + 1);
        std::string err;
        bool ok = false;
        if (self->viewportHost_.isNativeVideoActive() && self->viewportHost_.hasNativeVideoFrame()) {
            ok = self->viewportHost_.captureNativeScreenshot(fullPath, err);
        }
        if (!ok) {
            ok = self->bridge_->captureScreenshot(fullPath, err);
        }
        EnterCriticalSection(&self->auxLock_);
        self->pendingScreenshotToast_ = ok
            ? (std::string("Screenshot saved -> screenshots\\") + fname)
            : ("Screenshot failed: " + err);
        LeaveCriticalSection(&self->auxLock_);
        job->ctx->postMsg(MSG_SCREENSHOT_DONE, 0, 0);
        job->ctx->release();
    }
    delete job;
    return 0;
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

    // N36: offload GPU readback + PNG encoding / adb screencap to worker thread!
    showToast("Saving screenshot...", 1200);
    asyncCtx_->addRef();
    HANDLE hShot = CreateThread(NULL, 0, screenshotThreadProc,
                                new SimpleAsyncJob{ this, asyncCtx_, fullPath, 0 }, 0, NULL);
    if (hShot) CloseHandle(hShot);
    else asyncCtx_->release();
}

DWORD WINAPI ShellWindow::probeThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (!job || !job->ctx) {
        delete job;
        return 0;
    }
    ShellWindow* self = job->shell;
    int trials = (job->intArg >= 5) ? job->intArg : 60;

    auto publishProgress = [&](const ProbeResult& pr) {
        EnterCriticalSection(&self->auxLock_);
        self->probeResult_ = pr;
        LeaveCriticalSection(&self->auxLock_);
        job->ctx->postMsg(MSG_PROBE_UPDATE, 0, 0);
    };

    ProbeResult res;
    res.running = true;
    res.total = trials;
    res.summaryLine = "Preparing show_touches probe...";
    publishProgress(res);

    // 1. Save & enable show_touches on device
    std::string origVal;
    self->bridge_->runAdbShell("settings get system show_touches", origVal, 4000);
    while (!origVal.empty() && (origVal.back() == '\r' || origVal.back() == '\n' || origVal.back() == ' ')) {
        origVal.pop_back();
    }
    std::string restoreVal = (origVal == "1") ? "1" : "0";
    self->bridge_->runAdbShell("settings put system show_touches 1", origVal, 4000);
    Sleep(150);

    // 2. Pick darkest point on screen from 3x3 sample grid so the white touch indicator has high contrast
    float gx = 0.50f, gy = 0.50f;
    float gridY[9] = {0};
    if (self->viewportHost_.sampleGridBrightness(gridY)) {
        const float coords[3] = { 0.25f, 0.50f, 0.75f };
        int bestIdx = 4;
        for (int i = 0; i < 9; ++i) {
            if (gridY[i] < gridY[bestIdx]) bestIdx = i;
        }
        gx = coords[bestIdx % 3];
        gy = coords[bestIdx / 3];
    }

    self->viewportHost_.setProbeSampleRect(true, gx, gy);
    ResetEvent(self->probeEvent_);
    const ULONGLONG warmDeadline = GetTickCount64() + 500;
    while (GetTickCount64() < warmDeadline && !self->stopProbe_ && !self->stopAuxThreads_) {
        float wy = 0.0f;
        int64_t wqpc = 0;
        uint64_t wseq = 0;
        if (self->viewportHost_.getLastProbeSample(wy, wqpc, wseq) && wseq > 0) {
            break;
        }
        WaitForSingleObject(self->probeEvent_, 40);
    }

    std::vector<double> samples;
    samples.reserve(static_cast<size_t>(trials));
    const uint64_t ptrId = 98; // Dedicated probe finger ID
    float diagPreY = 0.0f;
    float diagPeakY = 0.0f;

    for (int t = 0; t < trials && !self->stopProbe_ && !self->stopAuxThreads_ && self->bridge_->isConnected(); ++t) {
        float preY = 0.0f;
        int64_t dummyQpc = 0;
        uint64_t preSeq = 0;
        self->viewportHost_.getLastProbeSample(preY, dummyQpc, preSeq);
        if (t == 0) { diagPreY = preY; diagPeakY = preY; }

        ResetEvent(self->probeEvent_);
        const int64_t t0 = LatencyTelemetry::nowQpc();
        self->bridge_->sendTouchNorm(TOUCH_DOWN, ptrId, gx, gy);

        double hitMs = -1.0;
        const ULONGLONG deadline = GetTickCount64() + 450;
        while (GetTickCount64() < deadline && !self->stopProbe_ && !self->stopAuxThreads_) {
            WaitForSingleObject(self->probeEvent_, 40);
            float curY = 0.0f;
            int64_t presQpc = 0;
            uint64_t curSeq = 0;
            if (self->viewportHost_.getLastProbeSample(curY, presQpc, curSeq) && curSeq > preSeq) {
                if (curY > diagPeakY) diagPeakY = curY;
                if (curY >= preY + 8.0f) {
                    hitMs = LatencyTelemetry::qpcToMs(presQpc - t0);
                    break;
                }
            }
        }

        self->bridge_->sendTouchNorm(TOUCH_UP, ptrId, gx, gy);
        if (hitMs > 0.0) {
            samples.push_back(hitMs);
        }

        // Wait for touch indicator to clear before next trial
        const ULONGLONG coolDeadline = GetTickCount64() + 260;
        while (GetTickCount64() < coolDeadline && !self->stopProbe_ && !self->stopAuxThreads_) {
            WaitForSingleObject(self->probeEvent_, 40);
            float curY = 0.0f;
            int64_t presQpc = 0;
            uint64_t curSeq = 0;
            if (self->viewportHost_.getLastProbeSample(curY, presQpc, curSeq) && curY <= preY + 5.0f) {
                break;
            }
        }
        Sleep(80);

        res.completed = t + 1;
        if ((t + 1) % 5 == 0 || t + 1 == trials) {
            char pbuf[96];
            std::snprintf(pbuf, sizeof(pbuf), "Probing %d/%d (%zu hits)...", t + 1, trials, samples.size());
            res.summaryLine = pbuf;
            publishProgress(res);
        }
    }

    self->viewportHost_.setProbeSampleRect(false, 0.5f, 0.5f);
    std::string tmpOut;
    self->bridge_->runAdbShell("settings put system show_touches " + restoreVal, tmpOut, 4000);

    res.running = false;
    TelemetrySummary vsum = self->telemetry_.getSummary();
    if (!samples.empty()) {
        std::vector<double> sorted = samples;
        res.valid = true;
        res.p50Ms = LatencyTelemetry::computePercentile(sorted, 0.50);
        res.p95Ms = LatencyTelemetry::computePercentile(sorted, 0.95);
        res.p99Ms = LatencyTelemetry::computePercentile(sorted, 0.99);
        res.minMs = sorted.front();
        res.maxMs = sorted.back();
        std::string ser = self->bridge_->activeSerial();
        const char* transport = (ser.find(':') != std::string::npos) ? "WiFi" : "USB";
        char line[384];
        std::snprintf(line, sizeof(line),
                      "Probe(%s n=%zu/%d pt=%.2f,%.2f baseY=%.1f peakY=%.1f): p50=%.1fms p95=%.1fms p99=%.1fms min=%.1f max=%.1f | rx->pres %.1f/%.1fms rx->dec %.1f/%.1fms %.0ffps (samp=%u dec=%llu prs=%llu skp=%llu) | UI max=%.2fms stalls=%u | pres=%s",
                      transport, samples.size(), trials, gx, gy, diagPreY, diagPeakY,
                      res.p50Ms, res.p95Ms, res.p99Ms, res.minMs, res.maxMs,
                      vsum.rxToPresentP50Ms, vsum.rxToPresentP95Ms,
                      vsum.rxToDecodeP50Ms, vsum.rxToDecodeP95Ms, vsum.measuredFps,
                      vsum.sampleCount,
                      static_cast<unsigned long long>(vsum.decodedCount),
                      static_cast<unsigned long long>(vsum.presentedCount),
                      static_cast<unsigned long long>(vsum.skippedCount),
                      self->maxUiMsgMs_, self->uiStallCount_,
                      self->viewportHost_.presentationModeLabel().c_str());
        res.summaryLine = line;
    } else {
        char line[440];
        FrameTiming lastFt;
        bool hasLastFt = self->telemetry_.getLastTiming(lastFt);
        std::snprintf(line, sizeof(line),
                      "Probe saw 0 hits (pt=%.2f,%.2f baseY=%.1f peakY=%.1f | rx->pres %.1f/%.1fms rx->dec %.1f/%.1fms %.0ffps samp=%u dec=%llu prs=%llu skp=%llu | lastFt(%d):rx=%lld,in=%lld,dec=%lld,pc=%lld,pr=%lld | occ=%d rtv=%d bw=%d hr=0x%08lX | UI max=%.2fms stalls=%u | pres=%s)",
                      gx, gy, diagPreY, diagPeakY,
                      vsum.rxToPresentP50Ms, vsum.rxToPresentP95Ms,
                      vsum.rxToDecodeP50Ms, vsum.rxToDecodeP95Ms, vsum.measuredFps,
                      vsum.sampleCount,
                      static_cast<unsigned long long>(vsum.decodedCount),
                      static_cast<unsigned long long>(vsum.presentedCount),
                      static_cast<unsigned long long>(vsum.skippedCount),
                      hasLastFt ? 1 : 0,
                      static_cast<long long>(lastFt.qpcPktRecv),
                      static_cast<long long>(lastFt.qpcProcIn),
                      static_cast<long long>(lastFt.qpcDecoded),
                      static_cast<long long>(lastFt.qpcPresentCall),
                      static_cast<long long>(lastFt.qpcPresentRet),
                      self->viewportHost_.isOccluded() ? 1 : 0,
                      self->viewportHost_.hasRtv() ? 1 : 0,
                      self->viewportHost_.backbufW(),
                      static_cast<unsigned long>(self->viewportHost_.lastPresentHResult()),
                      self->maxUiMsgMs_, self->uiStallCount_,
                      self->viewportHost_.presentationModeLabel().c_str());
        res.valid = false;
        res.summaryLine = line;
    }
    if (self->settings_.mirrorAudio || self->forceAudioOn_) {
        AudioStats astats = self->audioPlayer_.getStats();
        const char* acName = (astats.codec == AUDIO_CODEC_RAW) ? "raw"
                           : (astats.codec == AUDIO_CODEC_AAC) ? "aac" : "none";
        char abuf[160];
        std::snprintf(abuf, sizeof(abuf),
                      " | aud=%s pkt=%llu pf=%llu uf=%llu drp=%llu buf=%.1fms wasapi=%d",
                      acName,
                      static_cast<unsigned long long>(astats.packetsReceived),
                      static_cast<unsigned long long>(astats.framesPushed),
                      static_cast<unsigned long long>(astats.underflowFrames),
                      static_cast<unsigned long long>(astats.droppedFrames),
                      astats.bufferedMs,
                      astats.wasapiActive ? 1 : 0);
        res.summaryLine += abuf;
    }
    self->bridge_->appendLog("[Probe] " + res.summaryLine);
    {
        std::string logPath = self->exeDir_ + "\\latency_probe.log";
        std::ofstream lf(logPath.c_str(), std::ios::app);
        if (lf.is_open()) {
            std::vector<std::string> rlogs = self->bridge_->getRecentLogs();
            for (size_t li = 0; li < rlogs.size(); ++li) {
                lf << rlogs[li] << "\n";
            }
            lf << res.summaryLine << "\n";
        }
    }

    publishProgress(res);
    InterlockedExchange(&self->probing_, 0);
    job->ctx->release();
    delete job;
    return 0;
}

void ShellWindow::startRoundTripProbe(int numTrials) {
    if (!bridge_->isConnected() || !viewportHost_.isNativeVideoActive()) {
        showToast("Connect phone in Native D3D11 mode before running Latency Probe", 2400);
        return;
    }
    if (InterlockedCompareExchange(&probing_, 1, 0) != 0) {
        showToast("Latency Probe already running...", 1500);
        return;
    }
    InterlockedExchange(&stopProbe_, 0);
    asyncCtx_->addRef();
    HANDLE hPr = CreateThread(NULL, 0, probeThreadProc,
                              new SimpleAsyncJob{ this, asyncCtx_, "", numTrials }, 0, NULL);
    if (hPr) CloseHandle(hPr);
    else {
        InterlockedExchange(&probing_, 0);
        asyncCtx_->release();
    }
    invalidateShell();
}

void ShellWindow::onDeviceClipboard(void* ctx, const std::string& utf8Text) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (!self || utf8Text.empty()) return;
    EnterCriticalSection(&self->auxLock_);
    self->pendingDeviceClip_ = utf8Text;
    LeaveCriticalSection(&self->auxLock_);
    if (self->hwnd_) {
        PostMessageA(self->hwnd_, MSG_DEVICE_CLIPBOARD, 0, 0);
    }
}

std::string ShellWindow::readWindowsClipboardUtf8() const {
    if (!OpenClipboard(hwnd_)) return "";
    std::string out;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        const wchar_t* wstr = reinterpret_cast<const wchar_t*>(GlobalLock(hData));
        if (wstr) {
            int bytes = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
            if (bytes > 1) {
                out.resize(static_cast<size_t>(bytes - 1));
                WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], bytes - 1, NULL, NULL);
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
    return out;
}

void ShellWindow::writeWindowsClipboardUtf8(const std::string& utf8Text, bool fromPhone) {
    if (utf8Text.empty()) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), static_cast<int>(utf8Text.size()), NULL, 0);
    if (wlen <= 0) return;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen + 1) * sizeof(wchar_t));
    if (!hMem) return;
    wchar_t* dst = reinterpret_cast<wchar_t*>(GlobalLock(hMem));
    if (!dst) { GlobalFree(hMem); return; }
    MultiByteToWideChar(CP_UTF8, 0, utf8Text.c_str(), static_cast<int>(utf8Text.size()), dst, wlen);
    dst[wlen] = L'\0';
    GlobalUnlock(hMem);

    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
            GlobalFree(hMem);
        } else {
            lastSyncedClip_ = utf8Text;
            if (fromPhone) {
                auto setZeroDwordFormat = [](const wchar_t* fmtName) {
                    UINT fmt = RegisterClipboardFormatW(fmtName);
                    if (fmt == 0) return;
                    HGLOBAL hFlag = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DWORD));
                    if (hFlag) {
                        if (!SetClipboardData(fmt, hFlag)) {
                            GlobalFree(hFlag);
                        }
                    }
                };
                setZeroDwordFormat(L"CanIncludeInClipboardHistory");
                setZeroDwordFormat(L"CanUploadToCloudClipboard");
                setZeroDwordFormat(L"ExcludeClipboardContentFromMonitorProcessing");
            }
        }
        CloseClipboard();
        clipGuard_.noteLocalClipboardWrite(GetClipboardSequenceNumber(), utf8Text);
    } else {
        GlobalFree(hMem);
    }
}

DWORD WINAPI ShellWindow::appWatcherThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (!job || !job->ctx) {
        delete job;
        return 0;
    }
    ShellWindow* self = job->shell;
    std::string lastPkg;
    bool lastSecure = false;
    int pollCycle = 0;
    while (!self->stopAuxThreads_ && job->ctx->alive) {
        bool connected = self->bridge_->isConnected();
        bool inAim = (self->aimModeAtomic_ != 0);
        if (connected && !inAim) {
            std::string pkg;
            bool sec = false;
            self->bridge_->queryForegroundInfo(pkg, sec);
            bool changed = (!pkg.empty() && pkg != lastPkg) || (sec != lastSecure);
            if (changed && !self->stopAuxThreads_ && job->ctx->alive) {
                if (!pkg.empty()) lastPkg = pkg;
                lastSecure = sec;
                EnterCriticalSection(&self->auxLock_);
                self->pendingForegroundPkg_ = pkg;
                self->pendingSecureFlag_ = sec;
                LeaveCriticalSection(&self->auxLock_);
                job->ctx->postMsg(MSG_FOREGROUND_APP, 0, 0);
            }
        } else if (!connected) {
            lastPkg.clear();
            lastSecure = false;
        }
        // LATENCY 2.5: refresh battery temp & thermal status every ~10s off the UI thread
        if (connected && !inAim && (++pollCycle % 3) == 0 && !self->stopAuxThreads_ && job->ctx->alive) {
            PhoneThermalInfo ti;
            if (self->bridge_->queryThermalTelemetry(ti)) {
                EnterCriticalSection(&self->auxLock_);
                self->thermalInfo_ = ti;
                LeaveCriticalSection(&self->auxLock_);
                job->ctx->postMsg(MSG_THERMAL_UPDATE, 0, 0);
            }
        }
        // Poll less often while in a game profile (4.0s vs 2.0s) so adb dumpsys never stutters Android system_server!
        int ticks = (self->gameProfileAtomic_ != 0) ? 80 : 40;
        for (int i = 0; i < ticks && !self->stopAuxThreads_ && job->ctx->alive; ++i) {
            Sleep(50);
        }
    }
    job->ctx->release();
    delete job;
    return 0;
}

std::string ShellWindow::ensureDesktopProfileExists() {
    std::string desktopPath = profilesDir_ + "\\desktop.cfg";
    if (GetFileAttributesA(desktopPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Profile desk = ProfileDefaults::createDesktop("Desktop (Typing)");
        ProfileStore::save(desktopPath, desk);
    }
    return desktopPath;
}

bool ShellWindow::switchProfilePath(const std::string& targetPath, bool showAutoToast) {
    if (targetPath.empty()) return false;
    if (lstrcmpiA(profilePath_.c_str(), targetPath.c_str()) == 0) return true;

    if (profileDirty_) saveCurrentProfile(true);
    Profile loaded;
    if (!ProfileStore::load(targetPath, loaded)) return false;

    if (profile_.keymapEnabled && !profilePath_.empty()) {
        lastGameProfilePath_ = profilePath_;
    }

    profilePath_ = targetPath;
    profile_ = loaded;
    profileDirty_ = false;
    InterlockedExchange(&gameProfileAtomic_, profile_.keymapEnabled ? 1 : 0);
    lastHudSig_.valid = false;
    lastTopBarSig_.valid = false;
    if (profile_.keymapEnabled) {
        lastGameProfilePath_ = profilePath_;
        if (settings_.lastGameProfile != profilePath_) {
            settings_.lastGameProfile = profilePath_;
            AppSettingsStore::save(settingsPath_, settings_);
        }
    }
    hudOverlay_.setSelectedBtnIndex(-1);
    hudOverlay_.cancelRebind();
    hudOverlay_.clearUndo();
    recordRecentProfile(profilePath_);
    if (pendingUndoKind_ == UNDO_HUD_EDIT || pendingUndoKind_ == UNDO_STAY_FOR_APP) {
        pendingUndoKind_ = UNDO_NONE;
    }
    if (!profile_.keymapEnabled && aimMode_) {
        setAimMode(false);
    }
    ProfileStore::saveLastActive(profilesDir_, profilePath_);
    engine_->setProfile(profile_);
    hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
    drawerPanel_.setProfilesDir(profilesDir_, profilePath_);

    if (showAutoToast) {
        std::string msg = profile_.keymapEnabled
            ? ("Auto-switched -> " + profile_.name)
            : ("Desktop Mode · Typing active (" + activeAppPackage_ + ")");
        showToast(msg, 1800);
    }
    hudOverlay_.invalidate();
    invalidateShell();
    return true;
}

void ShellWindow::handleForegroundAppChanged(const std::string& pkg) {
    if (pkg.empty()) return;
    std::string prevPkg = activeAppPackage_;
    activeAppPackage_ = pkg;
    if (!manualOverridePkg_.empty() && pkg != manualOverridePkg_) {
        manualOverridePkg_.clear();
    }

    if (drawerOpen_ && drawerPanel_.activeTab() == 2) {
        invalidateShell();
    }
    if (!settings_.autoSwitchProfiles || drawerOpen_ || editMode_ || demoMode_) {
        return;
    }
    if (!manualOverridePkg_.empty() && pkg == manualOverridePkg_) {
        return;
    }

    // Already on a profile explicitly linked to this app
    if (ProfileDefaults::matchesPackage(profile_.appPackage, pkg)) {
        if (profile_.keymapEnabled) sessionAppProfiles_[pkg] = profilePath_;
        return;
    }

    // First detection right after launch: never yank user away from a manually chosen unlinked game profile
    if (prevPkg.empty() && profile_.keymapEnabled && profile_.appPackage.empty()) {
        sessionAppProfiles_[pkg] = profilePath_; // E5: remember which app used this unlinked profile!
        return;
    }

    // Search for a saved profile linked to pkg (prefer lastGameProfilePath_ first!)
    std::string matchedPath;
    if (!lastGameProfilePath_.empty()) {
        Profile cand;
        if (ProfileStore::load(lastGameProfilePath_, cand) && cand.keymapEnabled &&
            ProfileDefaults::matchesPackage(cand.appPackage, pkg)) {
            matchedPath = lastGameProfilePath_;
        }
    }
    if (matchedPath.empty()) {
        std::vector<std::string> files = ProfileStore::listProfiles(profilesDir_);
        for (size_t i = 0; i < files.size(); ++i) {
            Profile cand;
            if (ProfileStore::load(files[i], cand) &&
                ProfileDefaults::matchesPackage(cand.appPackage, pkg)) {
                matchedPath = files[i];
                break;
            }
        }
    }

    // E5: Remember which unlinked profile the user chose for this app in the current session!
    if (matchedPath.empty()) {
        std::map<std::string, std::string>::const_iterator it = sessionAppProfiles_.find(pkg);
        if (it != sessionAppProfiles_.end() && !it->second.empty()) {
            Profile cand;
            if (ProfileStore::load(it->second, cand) && cand.keymapEnabled) {
                matchedPath = it->second;
            }
        }
    }

    if (!matchedPath.empty()) {
        switchProfilePath(matchedPath, true);
    } else if (profile_.keymapEnabled) {
        // Non-game / unlinked app (WhatsApp, Chrome, Launcher, Settings...) -> switch cleanly to Desktop Typing Mode!
        std::string deskPath = ensureDesktopProfileExists();
        switchProfilePath(deskPath, true);
    }
}

DWORD WINAPI ShellWindow::dropWorkerThreadProc(LPVOID param) {
    SimpleAsyncJob* job = static_cast<SimpleAsyncJob*>(param);
    if (!job || !job->ctx) {
        delete job;
        return 0;
    }
    ShellWindow* self = job->shell;
    std::vector<std::string> files;
    EnterCriticalSection(&self->auxLock_);
    files.swap(self->pendingDropFiles_);
    LeaveCriticalSection(&self->auxLock_);

    auto notifyToast = [&](const std::string& msg) {
        if (!job->ctx->alive) return;
        EnterCriticalSection(&self->auxLock_);
        self->pendingDropToast_ = msg;
        LeaveCriticalSection(&self->auxLock_);
        job->ctx->postMsg(MSG_DROP_TOAST, 0, 0);
    };

    for (size_t i = 0; i < files.size() && !self->stopAuxThreads_ && job->ctx->alive; ++i) {
        const std::string& path = files[i];
        size_t slash = path.find_last_of("\\/");
        std::string fname = (slash == std::string::npos) ? path : path.substr(slash + 1);
        size_t dot = fname.find_last_of('.');
        bool isApk = (dot != std::string::npos && lstrcmpiA(fname.c_str() + dot, ".apk") == 0);

        std::string resultMsg;
        if (isApk) {
            notifyToast("Installing " + fname + "...");
            self->bridge_->installApk(path, resultMsg);
            notifyToast(resultMsg);
        } else {
            notifyToast("Sending " + fname + " to Downloads...");
            self->bridge_->pushFileToDownloads(path, resultMsg);
            notifyToast(resultMsg);
        }
    }

    InterlockedExchange(&self->dropping_, 0);
    job->ctx->release();
    delete job;
    return 0;
}

void ShellWindow::startAsyncDropTransfer(const std::vector<std::string>& files) {
    if (files.empty() || !bridge_->isConnected()) {
        showToast("Connect a phone first to install APKs or push files", 2200);
        return;
    }
    if (InterlockedCompareExchange(&dropping_, 1, 0) != 0) {
        showToast("Another file transfer is already in progress...", 1800);
        return;
    }
    EnterCriticalSection(&auxLock_);
    pendingDropFiles_ = files;
    LeaveCriticalSection(&auxLock_);
    asyncCtx_->addRef();
    HANDLE hDrop = CreateThread(NULL, 0, dropWorkerThreadProc,
                                new SimpleAsyncJob{ this, asyncCtx_, "", 0 }, 0, NULL);
    if (hDrop) CloseHandle(hDrop);
    else {
        InterlockedExchange(&dropping_, 0);
        asyncCtx_->release();
    }
}

bool ShellWindow::handleDesktopKeyEvent(int vk, bool down) {
    // N8: never leak PC keystrokes/typing into phone when a game keymap profile is active!
    if (!bridge_->isConnected() || aimMode_ || editMode_ || profile_.keymapEnabled) return false;
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    // E1: only treat Ctrl shortcuts when Alt (AltGr) is NOT pressed!
    if (KeyCodes::isPureCtrlShortcut(ctrl, alt) && down) {
        if (vk == 'V') {
            std::string clip = readWindowsClipboardUtf8();
            if (!clip.empty()) {
                lastSyncedClip_ = clip;
                clipGuard_.shouldSendPcToPhone(clip, GetClipboardSequenceNumber());
                bridge_->sendSetClipboard(clip, true);
                showToast("Pasted clipboard to phone", 1000);
            }
            return true;
        }
        if (vk == 'C') {
            bridge_->sendGetClipboard(1 /* COPY */);
            return true;
        }
        if (vk == 'X') {
            bridge_->sendGetClipboard(2 /* CUT */);
            return true;
        }
        if (vk == 'A' || vk == 'Z') {
            uint32_t akc = KeyCodes::vkToAndroidKeycode(vk);
            if (akc != 0) {
                const uint32_t metaCtrl = 0x3000u; // AMETA_CTRL_ON | AMETA_CTRL_LEFT_ON
                bridge_->sendKeycode(0, akc, metaCtrl);
                bridge_->sendKeycode(1, akc, metaCtrl);
                return true;
            }
        }
    }

    // Navigation & editing keys (Enter, Backspace, Delete, Tab, Escape, Arrows, Home, End, PgUp/Dn)
    bool isNavOrEdit = (vk == VK_RETURN || vk == VK_BACK || vk == VK_DELETE || vk == VK_TAB ||
                        vk == VK_ESCAPE || vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
                        vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT);
    if (isNavOrEdit) {
        uint32_t akc = KeyCodes::vkToAndroidKeycode(vk);
        if (akc != 0) {
            uint32_t meta = 0;
            if (shift) meta |= 0x41u;   // AMETA_SHIFT_ON | AMETA_SHIFT_LEFT_ON
            if (KeyCodes::isPureCtrlShortcut(ctrl, alt)) meta |= 0x3000u; // AMETA_CTRL_ON | AMETA_CTRL_LEFT_ON
            bridge_->sendKeycode(down ? 0u : 1u, akc, meta);
            return true;
        }
    }
    return false;
}

void ShellWindow::sendPhoneText(const std::string& utf8) {
    if (!bridge_->isConnected() || utf8.empty()) return;
    if (ScrcpyBridge::isPureAscii(utf8)) {
        bridge_->sendText(utf8);
        return;
    }
    // F10 / B3: Non-ASCII text travels via SET_CLIPBOARD(..., paste=true); restore phone clipboard afterward.
    std::string preserve = !lastDeviceClip_.empty()
        ? lastDeviceClip_
        : (settings_.clipboardSync ? readWindowsClipboardUtf8() : lastSyncedClip_);
    clipGuard_.beginNonAsciiTyping(utf8, preserve, GetTickCount64());
    bridge_->sendSetClipboard(utf8, true);
}

bool ShellWindow::handleDesktopChar(wchar_t ch) {
    // N8: text typing only runs in Typing/Desktop profiles (keymapEnabled == false)
    if (!bridge_->isConnected() || aimMode_ || editMode_ || profile_.keymapEnabled) return false;
    if (ch < 32 || ch == 127) return false; // Control chars handled in WM_KEYDOWN
    // E1: allow AltGr (Ctrl+Alt) printable characters through! Only block pure Ctrl+<char>.
    if (KeyCodes::isPureCtrlShortcut((GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                     (GetKeyState(VK_MENU) & 0x8000) != 0)) {
        return false;
    }

    wchar_t buf[2] = { ch, L'\0' };
    int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, 1, NULL, 0, NULL, NULL);
    if (bytes <= 0) return false;
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, 1, &utf8[0], bytes, NULL, NULL);
    sendPhoneText(utf8);
    return true;
}

void ShellWindow::setAimMode(bool enabled) {
    if (enabled && !bridge_->isConnected() && !demoMode_) return;
    if (enabled && !profile_.keymapEnabled) {
        if (!lastGameProfilePath_.empty() && switchProfilePath(lastGameProfilePath_, false)) {
            manualOverridePkg_ = activeAppPackage_;
            if (!activeAppPackage_.empty()) {
                sessionAppProfiles_[activeAppPackage_] = profilePath_;
            }
            showToast("Game Profile Active: " + profile_.name + " · Press " +
                                  KeyCodes::toName(profile_.toggleAimVk) + " / F1 for Aim Lock", 2400);
            return;
        }
        showToast("Desktop Typing Mode active · Pick a Game profile in CFG", 1800);
        return;
    }
    if (enabled && editMode_) setEditMode(false);
    if (aimMode_ == enabled) return;

    aimMode_ = enabled;
    InterlockedExchange(&aimModeAtomic_, enabled ? 1 : 0);
    lastHudSig_.valid = false;
    lastTopBarSig_.valid = false;
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
        showToast("AIM LOCK ON  (" + KeyCodes::toName(profile_.toggleAimVk) + " or F1 to unlock)", 1200);
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
        showToast("CURSOR MODE  (KB & WASD stay active!)", 1200);
    }
    syncInputHookSnapshot();
    animator_.start(ANIM_HUD_MODE_PULSE, 1.0f, 0.0f, ANIM_MODE_PULSE_MS, EASE_OUT_CUBIC, &viewportRc_);
    syncAnimTimer();
    if (aimMode_) tooltipPopup_.hide();
    hudOverlay_.invalidate();
    invalidateShell();
}

void ShellWindow::setDrawerState(bool open, int tab) {
    if (open && fullscreen_) toggleFullscreen();
    if (open && aimMode_ && tab == 0) setAimMode(false);

    bool wasOpen = drawerOpen_;
    drawerOpen_ = open;
    if (open) {
        drawerPanel_.setActiveTab(tab);
        drawerPanel_.setProfilesDir(profilesDir_, profilePath_);
        if (tab != 0) {
            editTestLive_ = false;
            drawerPanel_.setEditTestLive(false);
        }
    } else {
        editTestLive_ = false;
        drawerPanel_.setEditTestLive(false);
        hudOverlay_.cancelRebind();
        drawerPanel_.clearFocus();
    }

    // N32 + F6: Keys tab (0) OR Pad tab (4) while binding a pad button enters Edit Mode so HUD nodes are clickable
    bool wantEdit = (drawerOpen_ &&
                     (drawerPanel_.activeTab() == 0 || (drawerPanel_.activeTab() == 4 && drawerPanel_.hasSelectedPadButton())) &&
                     !editTestLive_ && (bridge_->isConnected() || demoMode_));
    setEditMode(wantEdit);
    engine_->setShellMenuOpen(drawerOpen_ || activePopup_ != POPUP_NONE);
    layoutChildren();
    if (open && !wasOpen) {
        animator_.start(ANIM_DRAWER_SLIDE, static_cast<float>(metrics_.drawerW()), 0.0f, ANIM_BASE_MS, EASE_OUT_CUBIC, &drawerRc_);
        syncAnimTimer();
    }
    if (!open && wasOpen) {
        recheckAutoSwitchForCurrentApp();
    }
}

void ShellWindow::setEditMode(bool enabled) {
    if (enabled && aimMode_) setAimMode(false);
    if (editMode_ == enabled) return;

    editMode_ = enabled;
    engine_->setEditMode(editMode_);
    if (editMode_) {
        engine_->releaseAllTouches();
    } else {
        hudOverlay_.cancelRebind();
        if (profileDirty_) saveCurrentProfile(true);
    }
    hudOverlay_.setInteractiveEdit(editMode_, profile_.hudOpacity);
    syncInputHookSnapshot();
    invalidateShell();
    if (!editMode_) {
        recheckAutoSwitchForCurrentApp();
    }
}

void ShellWindow::toggleFullscreen() {
    if (!hwnd_) return;
    tooltipPopup_.hide();
    if (activePopup_ != POPUP_NONE) closePopup();
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
        DWORD st = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        st &= ~(WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(st));
        const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
        const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     mi.rcMonitor.left, mi.rcMonitor.top, monW, monH,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        if (viewportHost_.isNativeVideoActive()) {
            // 1.4(b): present straight onto top-level window for Hardware Independent Flip!
            viewportHost_.setFullscreenTarget(hwnd_, true, monW, monH);
        }
        showToast("Fullscreen Mode (Press F11 to exit)", 1800);
    } else {
        fullscreen_ = false;
        DWORD st = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        st |= (WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(st));
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
        if (viewportHost_.isNativeVideoActive()) {
            viewportHost_.setFullscreenTarget(hwnd_, false, 0, 0);
        }
    }
    layoutChildren();
}

void ShellWindow::toggleAlwaysOnTop() {
    settings_.alwaysOnTop = !settings_.alwaysOnTop;
    scheduleSettingsSave();
    if (hwnd_ && !fullscreen_) {
        SetWindowPos(hwnd_, settings_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    showToast(settings_.alwaysOnTop ? "Window Pinned: Always on Top" : "Window Unpinned", 1400);
    invalidateShell();
}

void ShellWindow::toggleFireMode() {
    profile_.fireMode = (profile_.fireMode + 1) % 3;
    applyTuningToEngine();
    const char* msg = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) ? "Fire Mode: DRAG-FIRE (Right)"
                    : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)  ? "Fire Mode: CLAW-LEFT (Separate)"
                                                                  : "Fire Mode: DUAL-DRAG";
    showToast(msg, 1800);
}

void ShellWindow::toggleHud() {
    profile_.showHudInPlay = !profile_.showHudInPlay;
    applyTuningToEngine();
    showToast(profile_.showHudInPlay ? "HUD Key Badges: VISIBLE" : "HUD Key Badges: HIDDEN", 1200);
}

RECT ShellWindow::shellToastRect() const {
    int vw = viewportRc_.right - viewportRc_.left;
    int vh = viewportRc_.bottom - viewportRc_.top;
    if (vw <= 0 || vh <= 0) {
        RECT empty = { 0, 0, 0, 0 };
        return empty;
    }
    int ph = metrics_.dp(38);
    RECT r = {
        viewportRc_.left + metrics_.dp(12),
        viewportRc_.bottom - ph - metrics_.dp(14),
        viewportRc_.right - metrics_.dp(12),
        viewportRc_.bottom - metrics_.dp(8)
    };
    return r;
}

void ShellWindow::showToast(const std::string& msg, DWORD durationMs, int toastKind) {
    if (bridge_->isConnected() || demoMode_) {
        shellToastMsg_.clear();
        shellToastActionLabel_.clear();
        shellToastKind_ = toastKind;
        ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
        shellToastWasVisible_ = false;
        hudOverlay_.showToast(msg, durationMs);
        return;
    }
    shellToastMsg_ = msg;
    shellToastActionLabel_.clear();
    shellToastKind_ = toastKind;
    ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
    shellToastExpireTick_ = GetTickCount() + durationMs;
    shellToastWasVisible_ = !msg.empty();
    if (hwnd_) {
        RECT tr = shellToastRect();
        animator_.start(ANIM_TOAST_ALPHA, 0.0f, 1.0f, ANIM_BASE_MS, EASE_OUT_CUBIC, &tr);
        syncAnimTimer();
        if (tr.right > tr.left && tr.bottom > tr.top) {
            InvalidateRect(hwnd_, &tr, FALSE);
        } else {
            invalidateShell();
        }
    }
}

void ShellWindow::showSnackbar(const std::string& msg, const std::string& actionLabel, DWORD durationMs, int toastKind) {
    if (bridge_->isConnected() || demoMode_) {
        shellToastMsg_.clear();
        shellToastActionLabel_.clear();
        shellToastKind_ = toastKind;
        ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
        shellToastWasVisible_ = false;
        hudOverlay_.showSnackbar(msg, actionLabel, durationMs);
        return;
    }
    shellToastMsg_ = msg;
    shellToastActionLabel_ = actionLabel;
    shellToastKind_ = toastKind;
    ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
    shellToastExpireTick_ = GetTickCount() + durationMs;
    shellToastWasVisible_ = !msg.empty();
    if (hwnd_) {
        RECT tr = shellToastRect();
        animator_.start(ANIM_TOAST_ALPHA, 0.0f, 1.0f, ANIM_BASE_MS, EASE_OUT_CUBIC, &tr);
        syncAnimTimer();
        if (tr.right > tr.left && tr.bottom > tr.top) {
            InvalidateRect(hwnd_, &tr, FALSE);
        } else {
            invalidateShell();
        }
    }
}

bool ShellWindow::triggerShellUndo() {
    if (pendingUndoKind_ == UNDO_STAY_FOR_APP && !manualSwitchedAppPkg_.empty()) {
        std::string pkg = manualSwitchedAppPkg_;
        pendingUndoKind_ = UNDO_NONE;
        manualSwitchedAppPkg_.clear();
        if (!ProfileDefaults::matchesPackage(profile_.appPackage, pkg)) {
            if (!profile_.appPackage.empty()) profile_.appPackage += ",";
            profile_.appPackage += pkg;
        }
        if (profile_.keymapEnabled) {
            sessionAppProfiles_[pkg] = profilePath_;
        }
        applyProfileToEngine(true);
        showToast("Pinned '" + profile_.name + "' -> " + pkg, 2200, TOAST_SUCCESS);
        return true;
    }
    if (pendingUndoKind_ == UNDO_TRASH_PROFILE &&
        !undoTrashPath_.empty() && !undoTrashOrigPath_.empty()) {
        std::string tp = undoTrashPath_;
        std::string op = undoTrashOrigPath_;
        pendingUndoKind_ = UNDO_NONE;
        undoTrashPath_.clear();
        undoTrashOrigPath_.clear();
        if (ProfileStore::restoreFromTrash(tp, op)) {
            drawerPanel_.refreshProfileList();
            showToast("Restored deleted config", 1800, TOAST_SUCCESS);
            invalidateShell();
            return true;
        }
        return false;
    }
    if (hudOverlay_.canUndo()) {
        pendingUndoKind_ = UNDO_NONE;
        shellToastMsg_.clear();
        shellToastActionLabel_.clear();
        ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
        shellToastWasVisible_ = false;
        return hudOverlay_.undo();
    }
    return false;
}

void ShellWindow::triggerQuickReset() {
    engine_->releaseAllTouches();
    showToast("All Virtual Touches Reset!", 1000);
}

// --- IHudOverlayHost callbacks ---

std::string ShellWindow::hudSpecialRebindLabel() const {
    return drawerPanel_.specialRebindLabel();
}

void ShellWindow::hudOnSelectionChanged(int newSelectedIdx) {
    // F6: If picking a HUD target node from Gamepad tab, bind it immediately and return to playable mode!
    if (newSelectedIdx >= 0 && drawerOpen_ && drawerPanel_.activeTab() == 4 && drawerPanel_.hasSelectedPadButton() &&
        newSelectedIdx < static_cast<int>(profile_.buttons.size())) {
        std::string lbl = profile_.buttons[newSelectedIdx].label;
        if (lbl.empty()) lbl = KeyCodes::toName(profile_.buttons[newSelectedIdx].vk);
        std::string toastMsg;
        if (drawerPanel_.bindSelectedPadButtonToNode(profile_, lbl, toastMsg)) {
            applyProfileToEngine(true);
            bool wantEdit = (drawerOpen_ && drawerPanel_.hasSelectedPadButton() && (bridge_->isConnected() || demoMode_));
            setEditMode(wantEdit);
            if (!toastMsg.empty()) showToast(toastMsg, 2000, TOAST_SUCCESS);
            invalidateShell();
            return;
        }
    }
    // Clicking a key node while on PROFILES tab immediately opens its Inspector in KEYS tab!
    if (newSelectedIdx >= 0 && drawerOpen_ && drawerPanel_.activeTab() == 2) {
        drawerPanel_.setActiveTab(0);
    }
    invalidateShell();
}

void ShellWindow::hudOnProfileChanged(bool commitSaveNow) {
    if (!commitSaveNow) {
        // S2(b): single lightweight HUD update on drag move without shell invalidation or disk I/O
        engine_->setProfile(profile_);
        hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
        profileDirty_ = true;
        lastHudSig_.valid = false;
        hudOverlay_.invalidate();
        return;
    }
    // N33: apply live + debounce async disk save 500ms after edit commit
    engine_->setProfile(profile_);
    hudOverlay_.applyOpacity(editMode_, profile_.hudOpacity);
    profileDirty_ = true;
    lastHudSig_.valid = false;
    lastTopBarSig_.valid = false;
    profileSaveDueTick_ = GetTickCount() + 500;
    hasPendingProfileSave_ = true;
    hudOverlay_.invalidate();
    invalidateShell();
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
    showToast(msg, durationMs);
}

void ShellWindow::handleDrawerResult(const DrawerActionResult& res) {
    if (!res.toastMessage.empty()) {
        showToast(res.toastMessage, 2200);
    }
    if (res.cancelRebind) {
        hudOverlay_.cancelRebind();
    }
    if (res.tuningOnlyModified && !res.profileModified) {
        applyTuningToEngine();
    } else if (res.profileModified) {
        applyProfileToEngine(!drawerPanel_.isDraggingSlider());
    }
    if (res.settingsModified) {
        scheduleSettingsSave();
        InterlockedExchange(&autoSwitchAtomic_, settings_.autoSwitchProfiles ? 1 : 0);
        engine_->setGamepadEnabled(settings_.gamepadEnabled);
        if (res.vsyncToggled) {
            viewportHost_.setVsync(settings_.vsyncEnabled);
        }
        // Apply Turn Phone Screen Off only when that toggle actually changed (never spam on bitrate/FPS edits!)
        if (settings_.turnScreenOff != lastAppliedScreenOff_) {
            lastAppliedScreenOff_ = settings_.turnScreenOff;
            if (bridge_->isConnected() && !bridge_->isVirtualDisplayActive()) {
                bridge_->setDisplayPower(!settings_.turnScreenOff);
            }
        }
    }
    if (res.requestRunLatencyProbe) {
        startRoundTripProbe(60);
    }
    if (res.requestSave) {
        saveCurrentProfile(false);
    }
    if (res.requestSwitchProfile && !res.switchProfilePath.empty()) {
        if (switchProfilePath(res.switchProfilePath, false)) {
            manualOverridePkg_ = activeAppPackage_;
            if (!activeAppPackage_.empty() && profile_.keymapEnabled) {
                sessionAppProfiles_[activeAppPackage_] = profilePath_;
            }
            if (!activeAppPackage_.empty() && settings_.autoSwitchProfiles &&
                !ProfileDefaults::matchesPackage(profile_.appPackage, activeAppPackage_)) {
                manualSwitchedAppPkg_ = activeAppPackage_;
                pendingUndoKind_ = UNDO_STAY_FOR_APP;
                showSnackbar("Switched to " + profile_.name, "Stay on for this app", 6000, TOAST_INFO);
            } else {
                showToast("Loaded config: " + profile_.name, 2000, TOAST_SUCCESS);
            }
        }
    }
    if (res.requestLinkCurrentApp && !activeAppPackage_.empty()) {
        if (!ProfileDefaults::matchesPackage(profile_.appPackage, activeAppPackage_)) {
            if (!profile_.appPackage.empty()) profile_.appPackage += ",";
            profile_.appPackage += activeAppPackage_;
        }
        applyProfileToEngine(true);
        showToast("Linked '" + profile_.name + "' -> " + activeAppPackage_, 2400);
    }
    if (res.requestUnlinkApp) {
        profile_.appPackage.clear();
        applyProfileToEngine(true);
        showToast("Unlinked app auto-switch for '" + profile_.name + "'", 2000);
    }
    if (res.requestCalibrateAspect && lastPhoneW_ >= 64 && lastPhoneH_ >= 64) {
        profile_.refAspect = static_cast<float>(lastPhoneW_) / static_cast<float>(lastPhoneH_);
        profile_.edgeAnchored = true;
        applyProfileToEngine(true);
        char b[96];
        std::snprintf(b, sizeof(b), "Calibrated Ref Aspect -> %.2f:1 (%ux%u)", profile_.refAspect, lastPhoneW_, lastPhoneH_);
        showToast(b, 2200);
    }
    if (res.requestAddNode) {
        Profile beforeAdd = profile_;
        KeyNode n;
        n.label = res.addNodeLabel.size() > 7 ? res.addNodeLabel.substr(0, 7) : res.addNodeLabel;
        n.vk = 'K';
        n.action = res.addNodeType;
        float offset = static_cast<float>((addNodeStagger_++ % 6) * 0.03f);
        n.x = clamp01(0.46f + offset);
        n.y = clamp01(0.54f + offset * 0.6f);
        n.x2 = (n.action == NODE_SKILL_CAST) ? 0.880f : ((n.action == NODE_SWIPE) ? clamp01(n.x + 0.14f) : n.x);
        n.y2 = (n.action == NODE_SKILL_CAST) ? 0.180f : n.y;
        n.durationMs = (n.action == NODE_SWIPE) ? 120 : 35;
        n.radius = (n.action == NODE_SKILL_CAST) ? 0.110f : 0.030f;
        profile_.buttons.push_back(n);
        int newIdx = static_cast<int>(profile_.buttons.size()) - 1;
        hudOverlay_.startNewNodeCapture(newIdx, beforeAdd);
        applyProfileToEngine(true);
        showToast("Added " + ProfileDefaults::actionTypeToHumanLabel(n.action) + "! Press ANY Key, Mouse Btn, or Scroll (Esc cancels).", 3200);
    }
    if (res.requestClearButtonsUndo) {
        if (profile_.buttons.empty()) {
            showToast("No extra keys to clear", 1400);
        } else {
            hudOverlay_.pushUndoSnapshot(profile_, "Clear extra keys");
            profile_.buttons.clear();
            hudOverlay_.setSelectedBtnIndex(-1);
            hudOverlay_.cancelRebind();
            applyProfileToEngine(true);
            pendingUndoKind_ = UNDO_HUD_EDIT;
            showSnackbar("Cleared extra keys", "Undo", 6000);
        }
    }
    if (res.requestToggleEditTest) {
        editTestLive_ = !editTestLive_;
        drawerPanel_.setEditTestLive(editTestLive_);
        showToast(editTestLive_ ? "Live Test active — play directly on phone!" : "Resumed Keymap Edit Mode", 1800);
    }
    if (res.requestCreateTemplate) {
        Profile np = ProfileDefaults::createTemplate(static_cast<ProfileTemplateKind>(res.templateKind));
        std::string fname;
        std::string fullPath = makeUniqueProfilePath(profilesDir_, np.name, fname);
        if (ProfileStore::save(fullPath, np)) {
            drawerPanel_.refreshProfileList();
            switchProfilePath(fullPath, false);
            showToast("Created template: " + np.name, 2200);
        } else {
            showToast("ERROR: Could not create template file", 2600);
        }
    }
    if (res.requestImportProfile) {
        wchar_t fileBuf[MAX_PATH * 2] = {0};
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"Phantom Touch Profiles (*.cfg)\0*.cfg\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = sizeof(fileBuf) / sizeof(fileBuf[0]);
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        std::wstring initDir = utf8ToWide(profilesDir_);
        if (!initDir.empty()) ofn.lpstrInitialDir = initDir.c_str();
        if (GetOpenFileNameW(&ofn)) {
            std::string savedPath, impErr;
            if (ProfileStore::importProfileFile(wideToUtf8(fileBuf), profilesDir_, savedPath, impErr)) {
                drawerPanel_.refreshProfileList();
                switchProfilePath(savedPath, false);
                showToast("Imported config: " + profile_.name, 2200);
            } else {
                showToast("Import failed: " + impErr, 3000);
            }
        }
    }
    if (res.requestExportProfile) {
        wchar_t fileBuf[MAX_PATH * 2] = {0};
        std::wstring defName = utf8ToWide(ProfileStore::sanitizeFilename(profile_.name) + ".cfg");
        if (!defName.empty() && defName.size() < MAX_PATH) {
            std::memcpy(fileBuf, defName.c_str(), (defName.size() + 1) * sizeof(wchar_t));
        }
        OPENFILENAMEW ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = L"Phantom Touch Profiles (*.cfg)\0*.cfg\0";
        ofn.lpstrDefExt = L"cfg";
        ofn.lpstrFile = fileBuf;
        ofn.nMaxFile = sizeof(fileBuf) / sizeof(fileBuf[0]);
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&ofn)) {
            std::string destPath = wideToUtf8(fileBuf);
            if (ProfileStore::save(destPath, profile_)) {
                showToast("Exported profile -> " + destPath, 2400);
            } else {
                showToast("Export failed: could not write file", 2800);
            }
        }
    }
    if (res.requestCopyProfileText) {
        if (writeClipboardUtf8(hwnd_, ProfileStore::serializeToString(profile_))) {
            showToast("Copied profile config text to clipboard", 2000);
        } else {
            showToast("Could not access Windows clipboard", 2000);
        }
    }
    if (res.requestDeleteProfileTrash && !res.deleteProfilePath.empty()) {
        std::string trashPath;
        if (ProfileStore::moveToTrash(res.deleteProfilePath, profilesDir_, trashPath)) {
            drawerPanel_.refreshProfileList();
            pendingUndoKind_ = UNDO_TRASH_PROFILE;
            undoTrashPath_ = trashPath;
            undoTrashOrigPath_ = res.deleteProfilePath;
            showSnackbar("Deleted " + res.deleteProfileName, "Undo", 6000);
        } else {
            showToast("Could not delete " + res.deleteProfileName, 2200);
        }
    }
    if (res.requestStartRebindNode && hudOverlay_.selectedBtnIndex() >= 0 &&
        hudOverlay_.selectedBtnIndex() < static_cast<int>(profile_.buttons.size())) {
        hudOverlay_.startRebind(HUD_REBIND_BUTTON);
        showToast("Press ANY Key, Mouse Btn (LMB..X2), Scroll, or Combo...", 3200);
    }
    if (res.requestDeleteSelectedNode && hudOverlay_.selectedBtnIndex() >= 0 &&
        hudOverlay_.selectedBtnIndex() < static_cast<int>(profile_.buttons.size())) {
        hudOverlay_.pushUndoSnapshot(profile_, "Delete key");
        profile_.buttons.erase(profile_.buttons.begin() + hudOverlay_.selectedBtnIndex());
        hudOverlay_.setSelectedBtnIndex(-1);
        hudOverlay_.cancelRebind();
        applyProfileToEngine(true);
        pendingUndoKind_ = UNDO_HUD_EDIT;
        showSnackbar("Key deleted", "Undo", 6000);
    }
    if (res.requestCloseDrawer) {
        setDrawerState(false, drawerPanel_.activeTab());
    }
    if (res.requestReconnectStream) {
        scheduleSettingsSave();
        std::string ser = bridge_->activeSerial();
        startAsyncConnect(ser);
    }
    if (res.requestDisconnectDevice) {
        disconnectDevice(true);
    }
    if (res.requestConnectWireless) {
        startAsyncWirelessConnect(res.wirelessAddr);
    }
    if (res.requestOpenOnboarding) {
        setDrawerState(false, drawerPanel_.activeTab());
        showOnboarding_ = true;
        onboardingView_.setStep(toolsReady_ ? OB_STEP_PREPARE : OB_STEP_TOOLS);
        syncHudAndVideo();
    }

    // N32 + F6: KEYS tab (0) or PAD tab (4) while picking a node enters Edit Mode
    bool wantEdit = (drawerOpen_ &&
                     (drawerPanel_.activeTab() == 0 || (drawerPanel_.activeTab() == 4 && drawerPanel_.hasSelectedPadButton())) &&
                     !editTestLive_ && (bridge_->isConnected() || demoMode_));
    if (wantEdit != editMode_) {
        setEditMode(wantEdit);
    }

    syncInputHookSnapshot();
    hudOverlay_.invalidate();
    invalidateShell();
}

void ShellWindow::handleTopBarAction(TopBarAction act) {
    switch (act) {
        case TB_OPEN_PROFILES:
            if (activePopup_ == POPUP_PROFILE_DROPDOWN) closePopup();
            else openPopup(POPUP_PROFILE_DROPDOWN);
            break;
        case TB_TOGGLE_AIM: setAimMode(!aimMode_); break;
        case TB_OPEN_HELP:
            if (showOnboarding_) {
                finishOnboarding();
            } else if (activePopup_ == POPUP_CHEAT_SHEET) {
                closePopup();
            } else {
                openPopup(POPUP_CHEAT_SHEET);
            }
            break;
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

// --- Win32 Window Procedure (hooks & WM_INPUT live on InputHookThread) ---

LRESULT CALLBACK ShellWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ShellWindow* self = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<ShellWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ShellWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT ShellWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const int64_t t0 = LatencyTelemetry::nowQpc();
    LRESULT res = dispatchMessage(hwnd, msg, wParam, lParam);
    const double ms = LatencyTelemetry::qpcToMs(LatencyTelemetry::nowQpc() - t0);
    const bool modalOsMsg = (msg == WM_CREATE || msg == WM_NCCREATE || msg == WM_DESTROY ||
                             msg == WM_NCDESTROY || msg == WM_SYSCOMMAND ||
                             msg == WM_NCLBUTTONDOWN || msg == WM_ENTERSIZEMOVE || msg == WM_EXITSIZEMOVE);
    if (!modalOsMsg) {
        if (ms > maxUiMsgMs_) maxUiMsgMs_ = ms;
        if (ms > 16.0) {
            ++uiStallCount_;
            char sbuf[128];
            std::snprintf(sbuf, sizeof(sbuf), "[S6-UI] SLOW msg=0x%04X took %.2f ms (stalls=%u)",
                          msg, ms, uiStallCount_);
            bridge_->appendLog(sbuf);
        }
    }
    return res;
}

LRESULT ShellWindow::dispatchMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
            mmi->ptMinTrackSize.x = metrics_.minWinW();
            mmi->ptMinTrackSize.y = metrics_.minWinH();
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
                const int b = metrics_.resizeBorder();
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
            RECT maxRc = topBar_.maximizeButtonRect();
            if (!IsRectEmpty(&maxRc) && ptIn(maxRc, cpt.x, cpt.y)) {
                return HTMAXBUTTON; // U3(a): Windows 11 Snap Layouts flyout
            }
            if (ptIn(topBarRc_, cpt.x, cpt.y) && !topBar_.isInteractivePoint(cpt.x, cpt.y)) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_NCMOUSEMOVE: {
            if (!fullscreen_ && wParam == HTMAXBUTTON) {
                RECT maxRc = topBar_.maximizeButtonRect();
                POINT cpt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                ScreenToClient(hwnd, &cpt);
                hoverX_ = cpt.x;
                hoverY_ = cpt.y;
                if (lastHoverRegion_ != 1 || lastHoverItemId_ != TB_MAXIMIZE) {
                    if (lastHoverItemId_ >= 0 && !IsRectEmpty(&lastHoverRc_)) {
                        InvalidateRect(hwnd, &lastHoverRc_, FALSE);
                    }
                    lastHoverRegion_ = 1;
                    lastHoverItemId_ = TB_MAXIMIZE;
                    lastHoverRc_ = maxRc;
                    if (!IsRectEmpty(&maxRc)) InvalidateRect(hwnd, &maxRc, FALSE);
                }
                return 0;
            }
            if (lastHoverRegion_ == 1 && lastHoverItemId_ == TB_MAXIMIZE) {
                if (!IsRectEmpty(&lastHoverRc_)) InvalidateRect(hwnd, &lastHoverRc_, FALSE);
                lastHoverRegion_ = 0;
                lastHoverItemId_ = -1;
                SetRectEmpty(&lastHoverRc_);
                hoverX_ = -1;
                hoverY_ = -1;
            }
            break;
        }

        case WM_NCLBUTTONDOWN:
            if (!fullscreen_ && wParam == HTMAXBUTTON) return 0;
            break;

        case WM_NCLBUTTONUP:
            if (!fullscreen_ && wParam == HTMAXBUTTON) {
                ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
            break;

        case WM_ACTIVATE: {
            bool active = (LOWORD(wParam) != WA_INACTIVE);
            GdiPlusDraw::setDwmBorderActive(hwnd, active);
            if (!active) {
                tooltipPopup_.hide();
                if (activePopup_ != POPUP_NONE) closePopup();
            }
            break;
        }

        case WM_DPICHANGED: {
            int newDpi = HIWORD(wParam);
            if (newDpi < 72) newDpi = 96;
            metrics_.dpi = newDpi;
            GdiPlusDraw::setActiveDpi(metrics_.dpi);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested && !fullscreen_) {
                SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            cacheW_ = 0;
            cacheH_ = 0;
            layoutChildren();
            hudOverlay_.invalidate();
            invalidateShell();
            return 0;
        }

        case WM_ENTERSIZEMOVE:
            inSizeMove_ = true;
            break;

        case WM_EXITSIZEMOVE:
            inSizeMove_ = false;
            layoutChildren();
            invalidateShell();
            return 0;

        case WM_MOVE:
            // S4: moving window only syncs screen rects (zero relayout, zero UI Present)
            tooltipPopup_.hide();
            if (activePopup_ != POPUP_NONE) closePopup();
            syncHudAndVideo();
            syncInputHookSnapshot();
            return 0;

        case WM_SIZE:
            tooltipPopup_.hide();
            if (activePopup_ != POPUP_NONE) closePopup();
            if (!inSizeMove_ || (GetTickCount() - lastResizeTick_) >= 16) {
                lastResizeTick_ = GetTickCount();
                layoutChildren();
            }
            return 0;

        case MSG_TOGGLE_AIM:        setAimMode(!aimMode_); return 0;
        case MSG_UNLOCK_AIM:        setAimMode(false); return 0;
        case MSG_LOCK_AIM:          setAimMode(true); return 0;
        case MSG_TOGGLE_CMD_PALETTE:
            if (activePopup_ == POPUP_CMD_PALETTE) closePopup();
            else openPopup(POPUP_CMD_PALETTE);
            return 0;
        case MSG_TOGGLE_CHEAT_SHEET:
            if (activePopup_ == POPUP_CHEAT_SHEET) closePopup();
            else openPopup(POPUP_CHEAT_SHEET);
            return 0;
        case MSG_PAD_NAV: {
            uint8_t mask = static_cast<uint8_t>(wParam);
            if (mask & PAD_NAV_GUIDE) {
                if (activePopup_ != POPUP_NONE) closePopup();
                else openPopup(POPUP_CMD_PALETTE);
            }
            if (activePopup_ != POPUP_NONE) {
                if (mask & PAD_NAV_UP)     handlePopupKeyDown(VK_UP);
                if (mask & PAD_NAV_DOWN)   handlePopupKeyDown(VK_DOWN);
                if (mask & PAD_NAV_SELECT) handlePopupKeyDown(VK_RETURN);
                if (mask & PAD_NAV_BACK)   handlePopupKeyDown(VK_ESCAPE);
            } else if (drawerOpen_) {
                if (mask & PAD_NAV_BACK) {
                    setDrawerState(false, drawerPanel_.activeTab());
                } else {
                    DrawerActionResult res;
                    int vk = (mask & (PAD_NAV_DOWN | PAD_NAV_UP)) ? VK_TAB
                           : (mask & PAD_NAV_LEFT)   ? VK_LEFT
                           : (mask & PAD_NAV_RIGHT)  ? VK_RIGHT
                           : (mask & PAD_NAV_SELECT) ? VK_SPACE : 0;
                    if (vk != 0 && drawerPanel_.onKeyDown(vk, profile_, settings_, hudOverlay_.selectedBtnIndex(), res)) {
                        handleDrawerResult(res);
                        syncInputHookSnapshot();
                        invalidateShell();
                    }
                }
            }
            return 0;
        }
        case MSG_TOGGLE_STUDIO:
            if (drawerOpen_ && drawerPanel_.activeTab() == 0) setDrawerState(false, 0);
            else setDrawerState(true, 0);
            return 0;
        case MSG_CYCLE_FIRE:        toggleFireMode(); return 0;
        case MSG_TOGGLE_HUD:        toggleHud(); return 0;
        case MSG_RESET_TOUCH:       triggerQuickReset(); return 0;
        case MSG_TOGGLE_FULLSCREEN: toggleFullscreen(); return 0;
        case MSG_TOGGLE_DEBUG_HUD:
            debugOverlayVisible_ = !debugOverlayVisible_;
            lastTopBarSig_.valid = false;
            invalidateShell();
            showToast(debugOverlayVisible_ ? "Latency/UI Debug Overlay: ON" : "Debug Overlay: OFF", 1200);
            return 0;
        case MSG_RUN_LATENCY_PROBE:
            startRoundTripProbe(autoProbeSamples_);
            return 0;
        case MSG_INJECT_VIDEO_STALL:
            bridge_->triggerVideoStallTest();
            showToast("Injected 300ms video stall test", 1400);
            return 0;

        case WM_DEVICECHANGE: {
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE ||
                wParam == DBT_DEVNODES_CHANGED) {
                deviceChangeDebounceTick_ = GetTickCount() + 350;
                hasPendingDeviceChange_ = true;
            }
            return TRUE;
        }

        case MSG_CONNECT_STEP: {
            if (!bridge_->isConnected() && !fullscreen_) {
                InvalidateRect(hwnd, &viewportRc_, FALSE);
            }
            return 0;
        }

        case MSG_REPAIR_TOOLS_DONE:
        case MSG_TOOLS_READY: {
            EnterCriticalSection(&auxLock_);
            toolsReady_ = pendingToolsOk_;
            if (toolsReady_) {
                tools_ = pendingTools_;
                adbDevices_ = pendingAdbDevices_;
            } else {
                hubStatusMsg_ = pendingToolsErr_;
            }
            LeaveCriticalSection(&auxLock_);
            if (!toolsReady_) {
                bridge_->appendLog("[Tools] " + hubStatusMsg_);
            } else {
                bridge_->appendLog("[Tools] scrcpy " + tools_.scrcpyVersion + " ready");
                if (adbDevices_.empty()) {
                    hubStatusMsg_ = "Plug in your Android phone (USB Debugging ON) or use Wireless ADB below.";
                } else {
                    hubStatusMsg_.clear();
                }
                if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
            }

            if (showOnboarding_) {
                bool hasAuth = false;
                bool hasUnauth = false;
                for (size_t i = 0; i < adbDevices_.size(); ++i) {
                    if (adbDevices_[i].state == "device") hasAuth = true;
                    else if (adbDevices_[i].state == "unauthorized") hasUnauth = true;
                }
                onboardingView_.setStep(OnboardingView::computeRecommendedStep(
                    onboardingView_.step(), toolsReady_, bridge_->isConnected(), hasAuth, hasUnauth));
            } else if (toolsReady_ && msg == MSG_TOOLS_READY && !adbDevices_.empty()) {
                for (size_t i = 0; i < adbDevices_.size(); ++i) {
                    if (adbDevices_[i].state == "device") {
                        startAsyncConnect(adbDevices_[i].serial);
                        break;
                    }
                }
            }
            invalidateShell();
            return 0;
        }
        case MSG_DEVICES_REFRESHED: {
            EnterCriticalSection(&auxLock_);
            adbDevices_ = pendingAdbDevices_;
            LeaveCriticalSection(&auxLock_);
            if (showOnboarding_) {
                bool hasAuth = false;
                bool hasUnauth = false;
                for (size_t i = 0; i < adbDevices_.size(); ++i) {
                    if (adbDevices_[i].state == "device") hasAuth = true;
                    else if (adbDevices_[i].state == "unauthorized") hasUnauth = true;
                }
                onboardingView_.setStep(OnboardingView::computeRecommendedStep(
                    onboardingView_.step(), toolsReady_, bridge_->isConnected(), hasAuth, hasUnauth));
            }
            if (!bridge_->isConnected() && !connecting_) {
                if (reconnecting_ && !reconnectCancelled_ && !lastConnectedWireless_ && !lastConnectedSerial_.empty()) {
                    for (size_t i = 0; i < adbDevices_.size(); ++i) {
                        if (adbDevices_[i].serial == lastConnectedSerial_ && adbDevices_[i].state == "device") {
                            lastConnectError_.clear();
                            startAsyncConnect(lastConnectedSerial_);
                            break;
                        }
                    }
                } else if (adbDevices_.empty()) {
                    hubStatusMsg_ = "Plug in your Android phone (USB Debugging ON) or use Wireless ADB below.";
                } else {
                    hubStatusMsg_.clear();
                }
            }
            invalidateShell();
            return 0;
        }
        case MSG_DISCONNECT_DONE: {
            EnterCriticalSection(&auxLock_);
            adbDevices_ = pendingAdbDevices_;
            LeaveCriticalSection(&auxLock_);
            if (wParam != 0) {
                if (adbDevices_.empty()) {
                    hubStatusMsg_ = "Plug in your Android phone (USB Debugging ON) or use Wireless ADB below.";
                } else {
                    hubStatusMsg_.clear();
                }
            }
            syncInputHookSnapshot();
            invalidateShell();
            return 0;
        }
        case MSG_SCREENSHOT_DONE: {
            std::string toast;
            EnterCriticalSection(&auxLock_);
            toast = pendingScreenshotToast_;
            LeaveCriticalSection(&auxLock_);
            if (!toast.empty()) {
                showToast(toast, 2400);
            }
            return 0;
        }
        case MSG_PROBE_UPDATE: {
            ProbeResult pr;
            EnterCriticalSection(&auxLock_);
            pr = probeResult_;
            LeaveCriticalSection(&auxLock_);
            if (!pr.running && pr.valid) {
                showToast(pr.summaryLine, 4000);
            }
            if (!pr.running && autoProbeOnConnect_) {
                autoProbeOnConnect_ = false;
                PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            }
            invalidateShell();
            return 0;
        }
        case MSG_THERMAL_UPDATE: {
            if (drawerOpen_ && drawerPanel_.activeTab() == 3) {
                InvalidateRect(hwnd, &drawerRc_, FALSE);
            }
            return 0;
        }

        case MSG_CONNECT_DONE: {
            hubStatusMsg_ = asyncResultOk_ ? "" : asyncResultMsg_;
            if (asyncResultOk_) {
                lastConnectError_.clear();
                lastConnectedSerial_ = bridge_->activeSerial();
                lastConnectedWireless_ = (lastConnectedSerial_.find(':') != std::string::npos);
                reconnecting_ = false;
                reconnectCancelled_ = false;
                reconnectAttempt_ = 0;
                userDisconnected_ = false;
                secureAppDetected_ = false;
                lastDeviceClip_.clear();
                appliedStreamSettings_ = settings_;
                hasAppliedStreamSettings_ = true;
                if (showOnboarding_ && onboardingView_.step() <= OB_STEP_CONNECT) {
                    onboardingView_.setStep(OB_STEP_PROFILE);
                }
            } else {
                lastConnectError_ = asyncResultMsg_;
                if (reconnecting_ && !reconnectCancelled_) {
                    ++reconnectAttempt_;
                    if (reconnectAttempt_ > 5) {
                        reconnecting_ = false;
                        hubStatusMsg_ = "Connection lost after 5 retries.";
                    } else {
                        reconnectNextTick_ = GetTickCount() + static_cast<DWORD>(ConnectionStateMachine::backoffMs(reconnectAttempt_ - 1));
                    }
                }
            }
            std::string discSer, discEnc;
            EnterCriticalSection(&auxLock_);
            discSer = pendingDiscoveredSerial_;
            discEnc = pendingDiscoveredEncoder_;
            pendingDiscoveredSerial_.clear();
            pendingDiscoveredEncoder_.clear();
            LeaveCriticalSection(&auxLock_);
            if (asyncResultOk_ && !discSer.empty() && !discEnc.empty() &&
                settings_.encoderForDevice(discSer).empty()) {
                settings_.setEncoderForDevice(discSer, discEnc);
                scheduleSettingsSave();
            }
            if (!asyncResultOk_) {
                refreshAdbDevices();
            } else if (nativeVideoSession_) {
                uint32_t pw = 1920, ph = 1080;
                bridge_->getScreenSize(pw, ph);
                lastPhoneW_ = pw;
                lastPhoneH_ = ph;
                if (pw >= 64 && ph >= 64 &&
                    (settings_.lastPhoneW != static_cast<int>(pw) || settings_.lastPhoneH != static_cast<int>(ph))) {
                    settings_.lastPhoneW = static_cast<int>(pw);
                    settings_.lastPhoneH = static_cast<int>(ph);
                    scheduleSettingsSave();
                }
                std::string d3dErr;
                if (viewportHost_.enableNativeVideo(pw, ph, d3dErr, settings_.vsyncEnabled)) {
                    hudOverlay_.setEmbeddedD3DMode(true, ShellWindow::onHudBitmapReady, this);
                    sawEmbeddedMirror_ = true;
                    if (fullscreen_) {
                        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                        MONITORINFO mi;
                        ZeroMemory(&mi, sizeof(mi));
                        mi.cbSize = sizeof(mi);
                        if (GetMonitorInfoA(mon, &mi)) {
                            viewportHost_.setFullscreenTarget(hwnd_, true,
                                mi.rcMonitor.right - mi.rcMonitor.left,
                                mi.rcMonitor.bottom - mi.rcMonitor.top);
                        }
                    }
                    std::string modeTag = bridge_->isVirtualDisplayActive()
                        ? "Second-Phone Virtual Display (D3D11)"
                        : "Native D3D11 Single-Connection Stream";
                    if (settings_.clipboardSync && !settings_.clipboardNoticeShown) {
                        settings_.clipboardNoticeShown = true;
                        scheduleSettingsSave();
                        showToast("Clipboard sync is ON (phone copies excluded from Win+V history). Toggle in DEV.", 4500, TOAST_INFO);
                    } else {
                        showToast(modeTag + " · F2 Studio | " + KeyCodes::toName(profile_.toggleAimVk) + "/F1 Aim", 3600);
                    }
                    if (autoProbeOnConnect_ && probeRunCount_ == 0) {
                        autoProbeArmed_ = true;
                        autoProbeDueTick_ = GetTickCount() + 2000;
                    }
                } else {
                    bridge_->appendLog("[D3D11] Fallback to scrcpy.exe: " + d3dErr);
                    startAsyncLegacyFallback("D3D11 unavailable (" + d3dErr + ").");
                    return 0;
                }
            }
            syncInputHookSnapshot();
            layoutChildren();
            invalidateShell();
            return 0;
        }
        case MSG_DEVICE_LOST: {
            std::string rebuildErr;
            if (viewportHost_.isNativeVideoActive()) {
                if (viewportHost_.rebuildNativeDevice(rebuildErr)) {
                    InterlockedExchange(&reinitDecoderFlag_, 1);
                    bridge_->sendResetVideo();
                    hudOverlay_.invalidate();
                    bridge_->appendLog("[D3D11] GPU device reset recovered seamlessly.");
                } else {
                    bridge_->appendLog("[D3D11] Device rebuild failed: " + rebuildErr);
                    startAsyncLegacyFallback("GPU reset failed.");
                }
            }
            InterlockedExchange(&deviceResetPending_, 0);
            return 0;
        }
        case MSG_VIDEO_FALLBACK: {
            startAsyncLegacyFallback("Hardware/software video decoder unavailable.");
            return 0;
        }
        case MSG_QR_PAIR_UPDATE: {
            std::string autoAddr;
            EnterCriticalSection(&auxLock_);
            autoAddr = qrAutoConnectAddr_;
            qrAutoConnectAddr_.clear();
            LeaveCriticalSection(&auxLock_);
            if (!autoAddr.empty()) {
                startAsyncWirelessConnect(autoAddr);
            } else {
                invalidateShell();
            }
            return 0;
        }
        case MSG_WIRELESS_DONE: {
            hubStatusMsg_ = asyncResultMsg_;
            refreshAdbDevices();
            if (asyncResultOk_) {
                startAsyncConnect(pendingWirelessIp_);
            } else {
                showToast(asyncResultMsg_, 2600);
            }
            invalidateShell();
            return 0;
        }
        case MSG_DEVICE_CLIPBOARD: {
            std::string clip;
            EnterCriticalSection(&auxLock_);
            clip.swap(pendingDeviceClip_);
            LeaveCriticalSection(&auxLock_);
            if (settings_.clipboardSync && !clip.empty()) {
                lastDeviceClip_ = clip;
                if (clipGuard_.shouldApplyPhoneToPc(clip, GetTickCount64())) {
                    writeWindowsClipboardUtf8(clip, true);
                }
            }
            return 0;
        }
        case MSG_FOREGROUND_APP: {
            std::string pkg;
            bool sec = false;
            EnterCriticalSection(&auxLock_);
            pkg = pendingForegroundPkg_;
            sec = pendingSecureFlag_;
            LeaveCriticalSection(&auxLock_);
            bool secRising = (sec && !secureAppDetected_);
            secureAppDetected_ = sec;
            if (secRising) {
                showToast("This app blocks screen capture.", 4500);
            }
            if (!pkg.empty() && settings_.autoSwitchProfiles) {
                handleForegroundAppChanged(pkg);
            }
            if (secRising || !pkg.empty()) {
                lastTopBarSig_.valid = false;
                InvalidateRect(hwnd, &topBarRc_, FALSE);
            }
            return 0;
        }
        case MSG_DROP_TOAST: {
            std::string msgText;
            EnterCriticalSection(&auxLock_);
            msgText = pendingDropToast_;
            LeaveCriticalSection(&auxLock_);
            if (!msgText.empty()) {
                showToast(msgText, 2600);
            }
            return 0;
        }
        case MSG_ANDROID_BACK:
            bridge_->sendAndroidNav(4 /* KEYCODE_BACK */);
            return 0;

        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFFu, NULL, 0);
            std::vector<std::string> cfgFiles;
            std::vector<std::string> dropped;
            for (UINT i = 0; i < count; ++i) {
                wchar_t wpath[MAX_PATH * 2] = {0};
                if (DragQueryFileW(hDrop, i, wpath, sizeof(wpath) / sizeof(wpath[0]) - 1) > 0) {
                    std::string u8 = wideToUtf8(wpath);
                    if (!u8.empty()) {
                        if (isCfgFilePath(u8)) cfgFiles.push_back(u8);
                        else                   dropped.push_back(u8);
                    }
                }
            }
            DragFinish(hDrop);
            if (!cfgFiles.empty()) {
                int imported = 0;
                std::string lastSaved, lastErr;
                for (size_t i = 0; i < cfgFiles.size(); ++i) {
                    std::string savedPath, impErr;
                    if (ProfileStore::importProfileFile(cfgFiles[i], profilesDir_, savedPath, impErr)) {
                        ++imported;
                        lastSaved = savedPath;
                    } else {
                        lastErr = impErr;
                    }
                }
                if (imported > 0) {
                    drawerPanel_.refreshProfileList();
                    switchProfilePath(lastSaved, false);
                    showToast("Imported " + profile_.name + " (drag & drop)", 2400);
                } else if (!lastErr.empty()) {
                    showToast("Profile import failed: " + lastErr, 2800);
                }
            }
            if (!dropped.empty()) {
                startAsyncDropTransfer(dropped);
            }
            return 0;
        }

        case MSG_SENS_X_DOWN:
            profile_.sensX = clampRange(profile_.sensX - 0.05f, 0.10f, 10.0f);
            applyTuningToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens X: %.2f  (Y: %.2f)", profile_.sensX, profile_.sensY); showToast(b, 1200); }
            return 0;
        case MSG_SENS_X_UP:
            profile_.sensX = clampRange(profile_.sensX + 0.05f, 0.10f, 10.0f);
            applyTuningToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens X: %.2f  (Y: %.2f)", profile_.sensX, profile_.sensY); showToast(b, 1200); }
            return 0;
        case MSG_SENS_Y_DOWN:
            profile_.sensY = clampRange(profile_.sensY - 0.05f, 0.10f, 10.0f);
            applyTuningToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens Y: %.2f  (X: %.2f)", profile_.sensY, profile_.sensX); showToast(b, 1200); }
            return 0;
        case MSG_SENS_Y_UP:
            profile_.sensY = clampRange(profile_.sensY + 0.05f, 0.10f, 10.0f);
            applyTuningToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens Y: %.2f  (X: %.2f)", profile_.sensY, profile_.sensX); showToast(b, 1200); }
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_ANIM) {
                std::vector<RECT> dirty;
                if (animator_.tick(Animator::currentTimeMs(), dirty)) {
                    if (animator_.isActive(ANIM_HUD_MODE_PULSE) || animator_.value(ANIM_HUD_MODE_PULSE, 0.0f) > 0.0f) {
                        float pa = animator_.isActive(ANIM_HUD_MODE_PULSE)
                            ? animator_.value(ANIM_HUD_MODE_PULSE, 0.0f)
                            : 0.0f;
                        if (viewportHost_.isNativeVideoActive()) {
                            EngineTelemetry tel = engine_->getTelemetry();
                            DynamicHudMarkers dm;
                            dm.active = (!editMode_ && !showOnboarding_ && profile_.keymapEnabled &&
                                         (profile_.showHudInPlay || demoMode_));
                            dm.showCrosshair = (aimMode_ && profile_.showHudInPlay);
                            dm.aimFingerDown = tel.aimFingerDown;
                            dm.aimX = tel.aimFingerNormX;
                            dm.aimY = tel.aimFingerNormY;
                            dm.fireHeld = tel.fireHeld;
                            dm.joyDown = tel.joyDown;
                            dm.joyX = tel.joyFingerNormX;
                            dm.joyY = tel.joyFingerNormY;
                            dm.opacity = profile_.hudOpacity;
                            dm.modePulseAlpha = pa;
                            dm.modePulseAim = aimMode_;
                            viewportHost_.setDynamicHudMarkers(dm);
                        } else {
                            hudOverlay_.setModePulse(pa, aimMode_);
                        }
                    }
                    for (size_t di = 0; di < dirty.size(); ++di) {
                        InvalidateRect(hwnd, &dirty[di], FALSE);
                    }
                }
                syncAnimTimer();
                return 0;
            }
            if (wParam == TIMER_TICK) {
                bool fg = isAppForeground();
                engine_->setAppFocused(fg); // E4: gate XInput gamepad polling on window focus!
                if (!fg) {
                    if (aimMode_) setAimMode(false);
                    if (hadForeground_) engine_->releaseAllTouches();
                }
                hadForeground_ = fg;

                // F3: debounced WM_DEVICECHANGE wake
                DWORD nowTick = GetTickCount();
                if (hasPendingDeviceChange_ && static_cast<LONG>(nowTick - deviceChangeDebounceTick_) >= 0) {
                    hasPendingDeviceChange_ = false;
                    if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
                }

                // F3 / X2: Detect unexpected drop -> enter exponential backoff auto-reconnect
                if (sawEmbeddedMirror_ && !bridge_->isConnected() && !connecting_) {
                    bool unexpected = (!userDisconnected_ && !lastConnectedSerial_.empty());
                    disconnectDevice(true, !unexpected);
                    if (unexpected) {
                        reconnecting_ = true;
                        reconnectCancelled_ = false;
                        reconnectAttempt_ = 1;
                        reconnectNextTick_ = nowTick + static_cast<DWORD>(ConnectionStateMachine::backoffMs(0));
                        lastCountdownShown_ = -1;
                        hubStatusMsg_ = "Connection interrupted. Auto-reconnecting...";
                        if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
                    } else {
                        hubStatusMsg_ = "Phone disconnected. Reconnect USB cable or pick a device.";
                    }
                }

                // F3: Countdown refresh & backoff retry tick
                if (reconnecting_ && !reconnectCancelled_ && !connecting_ && !bridge_->isConnected()) {
                    int remSec = (nowTick < reconnectNextTick_)
                        ? static_cast<int>((reconnectNextTick_ - nowTick + 999) / 1000) : 0;
                    if (remSec != lastCountdownShown_) {
                        lastCountdownShown_ = remSec;
                        InvalidateRect(hwnd, &viewportRc_, FALSE);
                    }
                    if (static_cast<LONG>(nowTick - reconnectNextTick_) >= 0) {
                        if (reconnectAttempt_ > 5) {
                            reconnecting_ = false;
                            hubStatusMsg_ = "Connection lost after 5 reconnect attempts.";
                            invalidateShell();
                        } else if (lastConnectedWireless_) {
                            ++reconnectAttempt_;
                            reconnectNextTick_ = nowTick + static_cast<DWORD>(ConnectionStateMachine::backoffMs(reconnectAttempt_ - 1));
                            startAsyncWirelessConnect(lastConnectedSerial_);
                        } else {
                            bool foundReady = false;
                            for (size_t di = 0; di < adbDevices_.size(); ++di) {
                                if (adbDevices_[di].serial == lastConnectedSerial_ && adbDevices_[di].state == "device") {
                                    foundReady = true;
                                    break;
                                }
                            }
                            if (foundReady) {
                                startAsyncConnect(lastConnectedSerial_);
                            } else {
                                ++reconnectAttempt_;
                                reconnectNextTick_ = nowTick + static_cast<DWORD>(ConnectionStateMachine::backoffMs(reconnectAttempt_ - 1));
                                if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
                            }
                        }
                    }
                }

                syncHudAndVideo();
                hudOverlay_.checkToastExpired();
                if (shellToastWasVisible_ && static_cast<LONG>(nowTick - shellToastExpireTick_) >= 0) {
                    shellToastWasVisible_ = false;
                    shellToastMsg_.clear();
                    shellToastActionLabel_.clear();
                    ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
                    RECT tr = shellToastRect();
                    if (tr.right > tr.left && tr.bottom > tr.top) {
                        InvalidateRect(hwnd, &tr, FALSE);
                    }
                }
                // F10 / B3: Restore preserved phone clipboard 250ms after non-ASCII typing burst
                std::string restoreClip;
                if (clipGuard_.pollClipboardRestore(GetTickCount64(), restoreClip)) {
                    if (bridge_->isConnected()) {
                        bridge_->sendSetClipboard(restoreClip, false);
                    }
                }
                // V5: Debounced virtual display flex resize
                if (hasPendingVdResize_ && bridge_->isVirtualDisplayActive() &&
                    static_cast<LONG>(nowTick - pendingVdResizeTick_) >= 0) {
                    hasPendingVdResize_ = false;
                    int vw = viewportRc_.right - viewportRc_.left;
                    int vh = viewportRc_.bottom - viewportRc_.top;
                    if (vw >= 160 && vh >= 160) {
                        bridge_->sendResizeDisplay(static_cast<uint16_t>(vw), static_cast<uint16_t>(vh));
                    }
                }
                // N28: Debounced off-UI-thread profile save (500ms after last sensitivity/toggle press)
                if (hasPendingProfileSave_ && !profilePath_.empty() &&
                    static_cast<LONG>(nowTick - profileSaveDueTick_) >= 0) {
                    hasPendingProfileSave_ = false;
                    profileDirty_ = false;
                    ProfileSaveJob* job = new ProfileSaveJob{ profilePath_, profilesDir_, profile_ };
                    HANDLE hSave = CreateThread(NULL, 0, profileSaveWorkerProc, job, 0, NULL);
                    if (hSave) CloseHandle(hSave);
                    else { ProfileStore::save(job->path, job->profile); delete job; }
                }
                // N36: Debounced off-UI-thread settings save
                if (hasPendingSettingsSave_ && static_cast<LONG>(nowTick - settingsSaveDueTick_) >= 0 &&
                    InterlockedCompareExchange(&settingsSaveInFlight_, 1, 0) == 0) {
                    hasPendingSettingsSave_ = false;
                    SettingsSaveJob* sjob = new SettingsSaveJob{ settingsPath_, settings_, &settingsSaveInFlight_ };
                    HANDLE hSet = CreateThread(NULL, 0, settingsSaveWorkerProc, sjob, 0, NULL);
                    if (hSet) CloseHandle(hSet);
                    else {
                        AppSettingsStore::save(sjob->path, sjob->settings);
                        InterlockedExchange(&settingsSaveInFlight_, 0);
                        delete sjob;
                    }
                }
                // Automated round-trip probe (--probe-on-connect after warmup)
                if (autoProbeArmed_ && static_cast<LONG>(nowTick - autoProbeDueTick_) >= 0 &&
                    bridge_->isConnected() && viewportHost_.hasNativeVideoFrame()) {
                    autoProbeArmed_ = false;
                    ++probeRunCount_;
                    startRoundTripProbe(autoProbeSamples_);
                }
                bool connected = bridge_->isConnected();
                if (viewportHost_.isNativeVideoActive()) {
                    EngineTelemetry tel = engine_->getTelemetry();
                    DynamicHudMarkers dm;
                    dm.active = (!editMode_ && !showOnboarding_ && profile_.keymapEnabled &&
                                 (profile_.showHudInPlay || demoMode_));
                    dm.showCrosshair = (aimMode_ && profile_.showHudInPlay);
                    dm.aimFingerDown = tel.aimFingerDown;
                    dm.aimX = tel.aimFingerNormX;
                    dm.aimY = tel.aimFingerNormY;
                    dm.fireHeld = tel.fireHeld;
                    dm.joyDown = tel.joyDown;
                    dm.joyX = tel.joyFingerNormX;
                    dm.joyY = tel.joyFingerNormY;
                    dm.opacity = profile_.hudOpacity;
                    dm.modePulseAlpha = animator_.isActive(ANIM_HUD_MODE_PULSE)
                        ? animator_.value(ANIM_HUD_MODE_PULSE, 0.0f) : 0.0f;
                    dm.modePulseAim = aimMode_;
                    viewportHost_.setDynamicHudMarkers(dm);
                }
                if ((nowTick - lastTelemetryPaintTick_) >= 150) {
                    lastTelemetryPaintTick_ = nowTick;
                    if (!editMode_ && (connected || demoMode_)) {
                        // Dirty-check visual HUD state: ZERO GDI+ bitmap redraws when nothing changed!
                        // S5: Moving aim/joystick markers are rendered by D3D11 Pass 3 when native video is active.
                        bool hudUsesTelemetry = profile_.keymapEnabled &&
                                                (profile_.showHudInPlay || demoMode_ || (fullscreen_ && !aimMode_));
                        if (hudUsesTelemetry) {
                            EngineTelemetry tel = engine_->getTelemetry();
                            bool nativeVid = viewportHost_.isNativeVideoActive();
                            HudVisualSig cur;
                            cur.valid = true;
                            cur.aimMode = aimMode_;
                            cur.aimFingerDown = nativeVid ? false : tel.aimFingerDown;
                            cur.fireHeld = nativeVid ? false : tel.fireHeld;
                            cur.scopeActive = tel.scopeActive;
                            cur.holdViewActive = tel.holdViewActive;
                            cur.joyDown = nativeVid ? false : tel.joyDown;
                            cur.showHud = profile_.showHudInPlay;
                            cur.fullscreen = fullscreen_;
                            cur.aimQx = (!nativeVid && tel.aimFingerDown) ? static_cast<int>(tel.aimFingerNormX * 64.0f) : -1;
                            cur.aimQy = (!nativeVid && tel.aimFingerDown) ? static_cast<int>(tel.aimFingerNormY * 64.0f) : -1;
                            cur.fsRate = (fullscreen_ && !aimMode_) ? tel.touchRateHz : 0;
                            cur.activeBtnMask = tel.activeButtonMask;

                            bool changed = !lastHudSig_.valid ||
                                cur.aimMode != lastHudSig_.aimMode ||
                                cur.aimFingerDown != lastHudSig_.aimFingerDown ||
                                cur.fireHeld != lastHudSig_.fireHeld ||
                                cur.scopeActive != lastHudSig_.scopeActive ||
                                cur.holdViewActive != lastHudSig_.holdViewActive ||
                                cur.joyDown != lastHudSig_.joyDown ||
                                cur.showHud != lastHudSig_.showHud ||
                                cur.fullscreen != lastHudSig_.fullscreen ||
                                cur.aimQx != lastHudSig_.aimQx ||
                                cur.aimQy != lastHudSig_.aimQy ||
                                cur.fsRate != lastHudSig_.fsRate ||
                                cur.activeBtnMask != lastHudSig_.activeBtnMask;
                            if (changed) {
                                lastHudSig_ = cur;
                                hudOverlay_.invalidate();
                            }
                        }
                    }
                }

                // Dirty-checked TopBar telemetry + video latency refresh (max 2.5 Hz, and 0 Hz when nothing changed!)
                if (!fullscreen_ && (nowTick - lastTopBarPaintTick_) >= 400) {
                    lastTopBarPaintTick_ = nowTick;
                    EngineTelemetry tel = engine_->getTelemetry();
                    TelemetrySummary vsum = telemetry_.getSummary();
                    TopBarVisualSig cur;
                    cur.valid = true;
                    cur.connected = connected;
                    cur.reconnecting = reconnecting_;
                    cur.connLost = (!connected && !demoMode_ && !lastConnectError_.empty());
                    cur.aimMode = aimMode_;
                    cur.keymapEnabled = profile_.keymapEnabled;
                    cur.fireMode = profile_.fireMode;
                    cur.touchRateHz = tel.touchRateHz;
                    cur.latP50Q10 = static_cast<int>(vsum.rxToPresentP50Ms * 10.0 + 0.5);
                    cur.latP95Q10 = static_cast<int>(vsum.rxToPresentP95Ms * 10.0 + 0.5);
                    cur.skipped = vsum.skippedCount;

                    bool tbChanged = !lastTopBarSig_.valid ||
                        cur.connected != lastTopBarSig_.connected ||
                        cur.reconnecting != lastTopBarSig_.reconnecting ||
                        cur.connLost != lastTopBarSig_.connLost ||
                        cur.aimMode != lastTopBarSig_.aimMode ||
                        cur.keymapEnabled != lastTopBarSig_.keymapEnabled ||
                        cur.fireMode != lastTopBarSig_.fireMode ||
                        cur.touchRateHz != lastTopBarSig_.touchRateHz ||
                        cur.latP50Q10 != lastTopBarSig_.latP50Q10 ||
                        cur.latP95Q10 != lastTopBarSig_.latP95Q10 ||
                        cur.skipped != lastTopBarSig_.skipped;
                    if (tbChanged) {
                        lastTopBarSig_ = cur;
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
            if (drawerOpen_ && drawerPanel_.isDraggingSlider()) {
                DrawerActionResult res;
                if (drawerPanel_.onMouseMove(mx, my, profile_, hudOverlay_.selectedBtnIndex(), res)) {
                    handleDrawerResult(res);
                }
                InvalidateRect(hwnd, &drawerRc_, FALSE);
                return 0;
            } else if (cursorDirectDragging_ && !editMode_ && !aimMode_ && bridge_->isConnected()) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(mx, my, nx, ny, false)) {
                    lastCursorNormX_ = nx;
                    lastCursorNormY_ = ny;
                    engine_->onDirectCursorTouch(TOUCH_MOVE, nx, ny);
                }
                return 0;
            } else if (middlePanDragging_ && !editMode_ && !aimMode_ && bridge_->isConnected()) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(mx, my, nx, ny, false)) {
                    lastCursorNormX_ = nx;
                    lastCursorNormY_ = ny;
                    engine_->onDirectCursorTwoFingerPan(TOUCH_MOVE, nx, ny);
                }
                return 0;
            } else if (!editMode_ && !aimMode_ && bridge_->isConnected()) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(mx, my, nx, ny, true)) {
                    engine_->onCursorHoverNorm(nx, ny);
                }
            } else if (editMode_ && viewportHost_.isNativeVideoActive()) {
                int vx = 0, vy = 0;
                if (mapShellClientToVideoLocal(mx, my, vx, vy, !d3dHudDragging_)) {
                    hudOverlay_.dispatchMouseMove(vx, vy);
                } else if (!d3dHudDragging_) {
                    hudOverlay_.dispatchMouseLeave();
                }
            }

            // S2(a): Dirty-rect hover tracking — zero repaints while moving inside same element or viewport!
            int curRegion = 0;
            int curId = -1;
            RECT curRc = { 0, 0, 0, 0 };
            if (!fullscreen_ && ptIn(topBarRc_, mx, my)) {
                if (topBar_.hoverItemAt(mx, my, curId, curRc)) curRegion = 1;
            } else if (!fullscreen_ && ptIn(railRc_, mx, my)) {
                if (sideRail_.hoverItemAt(mx, my, curId, curRc)) curRegion = 2;
            } else if (!fullscreen_ && drawerOpen_ && ptIn(drawerRc_, mx, my)) {
                if (drawerPanel_.hoverItemAt(mx, my, curId, curRc)) curRegion = 3;
            } else if (showOnboarding_ && ptIn(viewportRc_, mx, my)) {
                if (onboardingView_.hoverItemAt(mx, my, curId, curRc)) curRegion = 5;
            } else if (!bridge_->isConnected() && !demoMode_ && ptIn(viewportRc_, mx, my)) {
                if (viewportHost_.hoverHubAt(mx, my, curId, curRc)) curRegion = 4;
            }

            if (curRegion != 0 || lastHoverItemId_ >= 0) {
                TRACKMOUSEEVENT tme;
                ZeroMemory(&tme, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }

            if (curRegion != lastHoverRegion_ || curId != lastHoverItemId_) {
                auto invalWithPad = [&](const RECT& r) {
                    if (r.right <= r.left || r.bottom <= r.top) return;
                    RECT d = { r.left - 2, r.top - 2, r.right + 2, r.bottom + 2 };
                    InvalidateRect(hwnd, &d, FALSE);
                };
                invalWithPad(lastHoverRc_);
                invalWithPad(curRc);
                animator_.start(ANIM_HOVER_FADE, 0.0f, 1.0f, ANIM_FAST_MS, EASE_OUT_CUBIC, &curRc);
                syncAnimTimer();
                lastHoverRegion_ = curRegion;
                lastHoverItemId_ = curId;
                lastHoverRc_ = curRc;
            }
            updateTooltipAtPoint(mx, my);
            return 0;
        }

        case WM_MOUSELEAVE:
            hoverX_ = -1;
            hoverY_ = -1;
            tooltipPopup_.hide();
            if (editMode_ && viewportHost_.isNativeVideoActive() && !d3dHudDragging_) {
                hudOverlay_.dispatchMouseLeave();
            }
            if (lastHoverItemId_ >= 0 && !IsRectEmpty(&lastHoverRc_)) {
                RECT d = { lastHoverRc_.left - 2, lastHoverRc_.top - 2, lastHoverRc_.right + 2, lastHoverRc_.bottom + 2 };
                InvalidateRect(hwnd, &d, FALSE);
            }
            lastHoverRegion_ = 0;
            lastHoverItemId_ = -1;
            SetRectEmpty(&lastHoverRc_);
            hoveringChrome_ = false;
            return 0;

        case WM_CAPTURECHANGED:
            // Only fires when capture is genuinely lost externally (e.g. Alt-Tab mid-drag).
            // Release at the finger's ACTUAL last position — never teleport to screen center (0.5, 0.5)!
            if (cursorDirectDragging_) {
                cursorDirectDragging_ = false;
                engine_->onDirectCursorTouch(TOUCH_UP, lastCursorNormX_, lastCursorNormY_);
            }
            if (middlePanDragging_) {
                middlePanDragging_ = false;
                engine_->onDirectCursorTwoFingerPan(TOUCH_UP, lastCursorNormX_, lastCursorNormY_);
            }
            if (d3dHudDragging_) {
                d3dHudDragging_ = false;
                hudOverlay_.dispatchMouseUp();
            }
            return 0;

        case WM_LBUTTONDBLCLK: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            if (editMode_ && viewportHost_.isNativeVideoActive()) {
                int vx = 0, vy = 0;
                if (mapShellClientToVideoLocal(mx, my, vx, vy, true)) {
                    hudOverlay_.dispatchDoubleClick(vx, vy);
                    return 0;
                }
            } else if (!editMode_ && !aimMode_ && bridge_->isConnected()) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(mx, my, nx, ny, true)) {
                    SetFocus(hwnd);
                    drawerPanel_.clearFocus();
                    editingHubWireless_ = false;
                    cursorDirectDragging_ = true;
                    lastCursorNormX_ = nx;
                    lastCursorNormY_ = ny;
                    SetCapture(hwnd);
                    engine_->onDirectCursorTouch(TOUCH_DOWN, nx, ny);
                    return 0;
                }
            }
            // N30: Double-clicks on chrome (TopBar / SideRail / Drawer / Hub) behave as a normal click!
            handleLeftButtonDown(hwnd, mx, my);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            handleLeftButtonDown(hwnd, mx, my);
            return 0;
        }

        case WM_LBUTTONUP: {
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            // CRITICAL: Complete drag/touch state BEFORE calling ReleaseCapture(), because
            // ReleaseCapture() synchronously dispatches WM_CAPTURECHANGED!
            if (cursorDirectDragging_) {
                cursorDirectDragging_ = false;
                float nx = lastCursorNormX_, ny = lastCursorNormY_;
                mapShellClientToVideoNorm(mx, my, nx, ny, false);
                lastCursorNormX_ = nx;
                lastCursorNormY_ = ny;
                engine_->onDirectCursorTouch(TOUCH_UP, nx, ny);
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            if (d3dHudDragging_) {
                d3dHudDragging_ = false;
                hudOverlay_.dispatchMouseUp();
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            if (GetCapture() == hwnd) ReleaseCapture();
            if (drawerOpen_ && drawerPanel_.onMouseUp()) {
                if (drawerPanel_.activeTab() == 1) {
                    applyTuningToEngine();
                    saveCurrentProfile(true);
                } else {
                    applyProfileToEngine(true);
                }
            }
            return 0;
        }

        case WM_MBUTTONDOWN: {
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) {
                hudOnMouseBindToDrawer(VK_MBUTTON);
                return 0;
            }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE && hudOverlay_.handleMouseBind(VK_MBUTTON)) return 0;
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            if (!editMode_ && !aimMode_ && bridge_->isConnected() &&
                profile_.toggleAimVk != VK_MBUTTON && !engine_->isKeyMapped(VK_MBUTTON, false)) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(mx, my, nx, ny, true)) {
                    SetFocus(hwnd);
                    drawerPanel_.clearFocus();
                    middlePanDragging_ = true;
                    lastCursorNormX_ = nx;
                    lastCursorNormY_ = ny;
                    SetCapture(hwnd);
                    engine_->onDirectCursorTwoFingerPan(TOUCH_DOWN, nx, ny);
                    return 0;
                }
            }
            break;
        }

        case WM_MBUTTONUP: {
            if (middlePanDragging_) {
                middlePanDragging_ = false;
                int mx = static_cast<short>(LOWORD(lParam));
                int my = static_cast<short>(HIWORD(lParam));
                float nx = lastCursorNormX_, ny = lastCursorNormY_;
                mapShellClientToVideoNorm(mx, my, nx, ny, false);
                lastCursorNormX_ = nx;
                lastCursorNormY_ = ny;
                engine_->onDirectCursorTwoFingerPan(TOUCH_UP, nx, ny);
                if (GetCapture() == hwnd) ReleaseCapture();
                return 0;
            }
            break;
        }

        case WM_XBUTTONDOWN: {
            int xvk = (HIWORD(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) { hudOnMouseBindToDrawer(xvk); return 0; }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE && hudOverlay_.handleMouseBind(xvk)) return 0;
            break;
        }

        case WM_RBUTTONDOWN: {
            if (drawerOpen_ && drawerPanel_.isWaitingSpecialRebind()) { hudOnMouseBindToDrawer(VK_RBUTTON); return 0; }
            if (hudOverlay_.rebindTarget() != HUD_REBIND_NONE && hudOverlay_.handleMouseBind(VK_RBUTTON)) return 0;
            int mx = static_cast<short>(LOWORD(lParam));
            int my = static_cast<short>(HIWORD(lParam));
            if (editMode_ && viewportHost_.isNativeVideoActive()) {
                int vx = 0, vy = 0;
                if (mapShellClientToVideoLocal(mx, my, vx, vy, true)) {
                    hudOverlay_.dispatchMouseDown(vx, vy, true);
                    return 0;
                }
            } else if (!editMode_ && !aimMode_ && bridge_->isConnected() && settings_.rightClickBack) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(mx, my, nx, ny, true)) {
                    SetFocus(hwnd);
                    if (drawerOpen_) { drawerPanel_.clearFocus(); invalidateShell(); }
                    bridge_->sendAndroidNav(4 /* KEYCODE_BACK */);
                    return 0;
                }
            }
            break;
        }

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
            if (editMode_ && viewportHost_.isNativeVideoActive()) {
                int vx = 0, vy = 0;
                if (mapShellClientToVideoLocal(pt.x, pt.y, vx, vy, true)) {
                    hudOverlay_.dispatchMouseWheel(vx, vy, delta);
                    return 0;
                }
            }
            if (drawerOpen_ && drawerPanel_.onMouseWheel(pt.x, pt.y, delta)) {
                invalidateShell();
                return 0;
            }
            if (!showOnboarding_ && !bridge_->isConnected() && !demoMode_ &&
                viewportHost_.onHubMouseWheel(pt.x, pt.y, delta)) {
                InvalidateRect(hwnd, &viewportRc_, FALSE);
                return 0;
            }
            if (!editMode_ && !aimMode_ && bridge_->isConnected()) {
                float nx = 0.0f, ny = 0.0f;
                if (mapShellClientToVideoNorm(pt.x, pt.y, nx, ny, true)) {
                    DWORD nowT = GetTickCount();
                    if ((delta > 0 && wheelDeltaAccum_ < 0) ||
                        (delta < 0 && wheelDeltaAccum_ > 0) ||
                        (nowT - lastWheelTick_) > 350u) {
                        wheelDeltaAccum_ = 0;
                    }
                    lastWheelTick_ = nowT;
                    wheelDeltaAccum_ += delta;
                    int notches = wheelDeltaAccum_ / WHEEL_DELTA;
                    wheelDeltaAccum_ -= notches * WHEEL_DELTA;
                    if (notches != 0) {
                        bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                        if (ctrlHeld) engine_->onDirectCursorPinch(nx, ny, notches);
                        else          engine_->onDirectCursorScroll(nx, ny, notches);
                    }
                    return 0;
                }
            }
            break;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            int vk = static_cast<int>(wParam);
            if (activePopup_ != POPUP_NONE) {
                handlePopupKeyDown(vk);
                return 0;
            }
            if (showOnboarding_ && onboardingView_.isEditingWifi()) {
                OnboardingClickResult obRes;
                if (onboardingView_.onKeyDown(vk, settings_.lastWirelessAddr, obRes)) {
                    scheduleSettingsSave();
                    if (obRes.action == OB_ACT_CONNECT_WIFI) {
                        startAsyncWirelessConnect(settings_.lastWirelessAddr);
                    }
                    syncInputHookSnapshot();
                    invalidateShell();
                    return 0;
                }
            }
            if (editingHubWireless_) {
                if (vk == VK_RETURN) {
                    editingHubWireless_ = false;
                    startAsyncWirelessConnect(settings_.lastWirelessAddr);
                } else if (vk == VK_ESCAPE) {
                    editingHubWireless_ = false;
                } else if (vk == VK_BACK && !settings_.lastWirelessAddr.empty()) {
                    settings_.lastWirelessAddr.pop_back();
                    scheduleSettingsSave();
                }
                syncInputHookSnapshot();
                invalidateShell();
                return 0;
            }
            if (drawerOpen_) {
                DrawerActionResult res;
                if (drawerPanel_.onKeyDown(vk, profile_, settings_, hudOverlay_.selectedBtnIndex(), res)) {
                    handleDrawerResult(res);
                    syncInputHookSnapshot();
                    invalidateShell();
                    return 0;
                }
            }
            if (editMode_ && hudOverlay_.handleKeyDown(vk)) return 0;
            if (!editMode_ && handleDesktopKeyEvent(vk, true)) return 0;
            if (!editMode_ && msg == WM_SYSKEYDOWN) return 0;
            break;
        }

        case WM_KEYUP: {
            int vk = static_cast<int>(wParam);
            if (!editMode_ && !editingHubWireless_ && !(drawerOpen_ && drawerPanel_.isTypingText())) {
                if (handleDesktopKeyEvent(vk, false)) return 0;
            }
            break;
        }

        case WM_IME_COMPOSITION: {
            if ((lParam & GCS_RESULTSTR) != 0 &&
                bridge_->isConnected() && !aimMode_ && !editMode_ && !profile_.keymapEnabled &&
                activePopup_ == POPUP_NONE && !editingHubWireless_ &&
                !(drawerOpen_ && drawerPanel_.isTypingText())) {
                HIMC hImc = ImmGetContext(hwnd);
                if (hImc) {
                    LONG bytes = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, NULL, 0);
                    if (bytes > 0) {
                        std::vector<wchar_t> wbuf(static_cast<size_t>(bytes) / sizeof(wchar_t) + 1u, L'\0');
                        LONG got = ImmGetCompositionStringW(hImc, GCS_RESULTSTR, wbuf.data(), bytes);
                        if (got > 0) {
                            const int wcharCnt = static_cast<int>(got / sizeof(wchar_t));
                            int u8Len = WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wcharCnt, NULL, 0, NULL, NULL);
                            if (u8Len > 0) {
                                std::string u8(static_cast<size_t>(u8Len), '\0');
                                WideCharToMultiByte(CP_UTF8, 0, wbuf.data(), wcharCnt, &u8[0], u8Len, NULL, NULL);
                                sendPhoneText(u8);
                            }
                        }
                    }
                    ImmReleaseContext(hwnd, hImc);
                    LPARAM rem = lParam & ~static_cast<LPARAM>(GCS_RESULTSTR);
                    return (rem == 0) ? 0 : DefWindowProcW(hwnd, msg, wParam, rem);
                }
            }
            break;
        }

        case WM_CLIPBOARDUPDATE: {
            if (!settings_.clipboardSync || !bridge_->isConnected()) return 0;
            if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return 0;
            const DWORD seq = GetClipboardSequenceNumber();
            const std::string clip = readWindowsClipboardUtf8();
            if (clipGuard_.shouldSendPcToPhone(clip, seq)) {
                lastSyncedClip_ = clip;
                bridge_->sendSetClipboard(clip, false);
            }
            return 0;
        }

        case WM_CHAR: {
            wchar_t ch = static_cast<wchar_t>(wParam);
            if (activePopup_ != POPUP_NONE) {
                handlePopupChar(ch);
                return 0;
            }
            if (showOnboarding_ && onboardingView_.isEditingWifi() && ch <= 126) {
                if (onboardingView_.onChar(static_cast<char>(ch), settings_.lastWirelessAddr)) {
                    scheduleSettingsSave();
                    invalidateShell();
                    return 0;
                }
            }
            if (editingHubWireless_ && ch >= 32 && ch <= 126) {
                if (settings_.lastWirelessAddr.size() < 28) {
                    settings_.lastWirelessAddr.push_back(static_cast<char>(ch));
                    scheduleSettingsSave();
                    invalidateShell();
                }
                return 0;
            }
            if (drawerOpen_) {
                DrawerActionResult res;
                if (ch <= 126 && drawerPanel_.onChar(static_cast<char>(ch), profile_, settings_, hudOverlay_.selectedBtnIndex(), res)) {
                    handleDrawerResult(res);
                    return 0;
                }
            }
            // B4: handle UTF-16 surrogate pairs (e.g. emoji / rare CJK) seamlessly for Unicode typing
            if (ch >= 0xD800 && ch <= 0xDBFF) {
                highSurrogate_ = ch;
                return 0;
            }
            if (ch >= 0xDC00 && ch <= 0xDFFF && highSurrogate_ != 0) {
                wchar_t pair[2] = { highSurrogate_, ch };
                highSurrogate_ = 0;
                if (bridge_->isConnected() && !aimMode_ && !editMode_ && !profile_.keymapEnabled &&
                    !KeyCodes::isPureCtrlShortcut((GetKeyState(VK_CONTROL) & 0x8000) != 0,
                                                  (GetKeyState(VK_MENU) & 0x8000) != 0)) {
                    int bytes = WideCharToMultiByte(CP_UTF8, 0, pair, 2, NULL, 0, NULL, NULL);
                    if (bytes > 0) {
                        std::string u8(static_cast<size_t>(bytes), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, pair, 2, &u8[0], bytes, NULL, NULL);
                        sendPhoneText(u8);
                    }
                    return 0;
                }
                break;
            }
            highSurrogate_ = 0;
            if (!editMode_ && handleDesktopChar(ch)) return 0;
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
            onPaint(hdc, ps.rcPaint);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            InterlockedExchange(&stopAuxThreads_, 1);
            InterlockedExchange(&stopProbe_, 1);
            if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
            flushOnExit();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            InterlockedExchange(&stopAuxThreads_, 1);
            InterlockedExchange(&stopProbe_, 1);
            if (deviceMonitorWakeEvent_) SetEvent(deviceMonitorWakeEvent_);
            if (asyncCtx_) {
                EnterCriticalSection(&asyncCtx_->lock);
                InterlockedExchange(&asyncCtx_->alive, 0);
                asyncCtx_->hwnd = NULL;
                LeaveCriticalSection(&asyncCtx_->lock);
            }
            flushOnExit();
            setAimMode(false);
            viewportHost_.detachMirror();
            tooltipPopup_.destroy();
            modalPopup_.destroy();
            if (clipboardListenerActive_) {
                RemoveClipboardFormatListener(hwnd);
                clipboardListenerActive_ = false;
            }
            KillTimer(hwnd, TIMER_TICK);
            KillTimer(hwnd, TIMER_ANIM);
            animTimerActive_ = false;
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShellWindow::handleLeftButtonDown(HWND hwnd, int mx, int my) {
    SetFocus(hwnd);
    editingHubWireless_ = false;

    if (activePopup_ != POPUP_NONE) {
        if (ptIn(topBarRc_, mx, my) && topBar_.hitTest(mx, my) == TB_OPEN_PROFILES &&
            activePopup_ == POPUP_PROFILE_DROPDOWN) {
            closePopup();
            return;
        }
        closePopup();
    }

    // 1. TopBar interactive controls
    if (ptIn(topBarRc_, mx, my)) {
        TopBarAction tb = topBar_.hitTest(mx, my);
        if (tb != TB_NONE) { handleTopBarAction(tb); return; }
    }
    // 2. SideRail emulator buttons
    if (ptIn(railRc_, mx, my)) {
        SideRailAction sr = sideRail_.hitTest(mx, my);
        if (sr != SR_NONE) { handleSideRailAction(sr); return; }
    }
    // 3. Docked Outside-Viewport DrawerPanel
    if (drawerOpen_ && drawerPanel_.containsPoint(mx, my)) {
        SetCapture(hwnd);
        int sel = hudOverlay_.selectedBtnIndex();
        DrawerActionResult res;
        if (drawerPanel_.onMouseDown(mx, my, profile_, settings_, sel, res)) {
            hudOverlay_.setSelectedBtnIndex(sel);
            handleDrawerResult(res);
            return;
        }
    }
    // Clicking outside drawer clears drawer text/rebind focus so keyboard typing targets the phone!
    if (drawerOpen_) {
        drawerPanel_.clearFocus();
        invalidateShell();
    }
    // 3b. Clickable Undo/Action Snackbar (both offline shell toast & HUD overlay snackbar!)
    if (!shellToastMsg_.empty() && !shellToastActionLabel_.empty() &&
        static_cast<LONG>(GetTickCount() - shellToastExpireTick_) < 0 &&
        ptIn(shellToastActionRc_, mx, my)) {
        if (triggerShellUndo()) return;
    }
    if (bridge_->isConnected() || demoMode_) {
        int vx = 0, vy = 0;
        if (mapShellClientToVideoLocal(mx, my, vx, vy, true) && hudOverlay_.hitTestToastAction(vx, vy)) {
            if (triggerShellUndo()) return;
        }
    }
    // 4. Native D3D11 Embedded Edit Mode Canvas (same-frame HUD interaction!)
    if (editMode_ && viewportHost_.isNativeVideoActive()) {
        int vx = 0, vy = 0;
        if (mapShellClientToVideoLocal(mx, my, vx, vy, true)) {
            d3dHudDragging_ = true;
            SetCapture(hwnd);
            hudOverlay_.dispatchMouseDown(vx, vy, false);
            return;
        }
    }
    // 5. Direct Cursor-Mode Touch on Phone Screen (Native D3D11 Viewport!)
    if (!editMode_ && !aimMode_ && bridge_->isConnected()) {
        float nx = 0.0f, ny = 0.0f;
        if (mapShellClientToVideoNorm(mx, my, nx, ny, true)) {
            cursorDirectDragging_ = true;
            lastCursorNormX_ = nx;
            lastCursorNormY_ = ny;
            SetCapture(hwnd);
            engine_->onDirectCursorTouch(TOUCH_DOWN, nx, ny);
            return;
        }
    }
    // 6. Onboarding Wizard OR Device Hub inside ViewportHost
    if (showOnboarding_ && !demoMode_ && ptIn(viewportRc_, mx, my)) {
        OnboardingClickResult ob;
        if (onboardingView_.onMouseDown(mx, my, ob)) {
            if (ob.action == OB_ACT_SKIP || ob.action == OB_ACT_FINISH) {
                finishOnboarding();
                return;
            }
            if (ob.action == OB_ACT_REPAIR_TOOLS) {
                startAsyncRepairTools();
            } else if (ob.action == OB_ACT_RECHECK_TOOLS) {
                if (toolsReady_) refreshAdbDevices();
                else startAsyncRepairTools();
            } else if (ob.action == OB_ACT_CONNECT_DEVICE && ob.index >= 0 &&
                       ob.index < static_cast<int>(adbDevices_.size())) {
                startAsyncConnect(adbDevices_[ob.index].serial);
            } else if (ob.action == OB_ACT_CONNECT_WIFI) {
                startAsyncWirelessConnect(settings_.lastWirelessAddr);
            } else if (ob.action == OB_ACT_TOGGLE_QR) {
                if (showingQrPair_) { stopQrWirelessPairing(); refreshAdbDevices(); }
                else startQrWirelessPairing();
            } else if (ob.action == OB_ACT_APPLY_PROFILE_CHOICE) {
                applyOnboardingProfileChoice(ob.profileChoice);
                animator_.start(ANIM_ONBOARD_PAGE, 0.0f, 1.0f,
                                ANIM_BASE_MS, EASE_OUT_CUBIC, &viewportRc_);
                syncAnimTimer();
            }
            syncInputHookSnapshot();
            invalidateShell();
            return;
        }
    }
    if (!bridge_->isConnected() && !demoMode_ && ptIn(viewportRc_, mx, my)) {
        HubClickResult hr = viewportHost_.hitTestHub(mx, my);
        if (hr.action == HUB_REFRESH_DEVICES) {
            if (!toolsReady_) {
                startAsyncRepairTools();
            } else {
                lastConnectError_.clear();
                refreshAdbDevices();
            }
        } else if (hr.action == HUB_CONNECT_DEVICE && hr.index >= 0 &&
                   hr.index < static_cast<int>(adbDevices_.size())) {
            reconnecting_ = false;
            reconnectCancelled_ = false;
            startAsyncConnect(adbDevices_[hr.index].serial);
        } else if (hr.action == HUB_FOCUS_WIFI_ADDR) {
            editingHubWireless_ = true;
            invalidateShell();
        } else if (hr.action == HUB_CONNECT_WIFI) {
            reconnecting_ = false;
            reconnectCancelled_ = false;
            startAsyncWirelessConnect(settings_.lastWirelessAddr);
        } else if (hr.action == HUB_TOGGLE_QR_PAIR) {
            if (showingQrPair_) { stopQrWirelessPairing(); refreshAdbDevices(); }
            else startQrWirelessPairing();
        } else if (hr.action == HUB_REPAIR_TOOLS) {
            startAsyncRepairTools();
        } else if (hr.action == HUB_OPEN_ONBOARDING) {
            showOnboarding_ = true;
            onboardingView_.setStep(toolsReady_ ? OB_STEP_PREPARE : OB_STEP_TOOLS);
            invalidateShell();
        } else if (hr.action == HUB_CANCEL_RECONNECT) {
            reconnecting_ = false;
            reconnectCancelled_ = true;
            lastConnectError_.clear();
            hubStatusMsg_ = "Auto-reconnect cancelled.";
            invalidateShell();
        } else if (hr.action == HUB_RETRY_RECONNECT) {
            lastConnectError_.clear();
            if (!lastConnectedSerial_.empty()) {
                reconnecting_ = true;
                reconnectCancelled_ = false;
                reconnectAttempt_ = 1;
                if (lastConnectedWireless_) startAsyncWirelessConnect(lastConnectedSerial_);
                else                        startAsyncConnect(lastConnectedSerial_);
            } else {
                refreshAdbDevices();
            }
        } else if (hr.action == HUB_COPY_ERROR) {
            std::string det = lastConnectError_;
            std::vector<std::string> rlogs = bridge_->getRecentLogs();
            for (size_t li = 0; li < rlogs.size(); ++li) {
                det += "\r\n" + rlogs[li];
            }
            if (!det.empty()) {
                writeWindowsClipboardUtf8(det);
                showToast("Copied error details to clipboard", 1800);
            }
        } else if (hr.action == HUB_START_OFFLINE_STUDIO) {
            stopQrWirelessPairing();
            reconnecting_ = false;
            demoMode_ = true;
            setDrawerState(true, 0);
            showToast("Offline Keymap Studio active! Changes auto-save to " + profile_.name, 3000);
        }
    }
}

bool ShellWindow::ensurePaintCache(HDC refDc, int w, int h) {
    if (!refDc || w <= 0 || h <= 0) return false;
    if (cacheDC_ && w == cacheW_ && h == cacheH_) return false;
    if (cacheDC_) {
        if (cacheOldBmp_) SelectObject(cacheDC_, cacheOldBmp_);
        if (cacheBmp_) DeleteObject(cacheBmp_);
        DeleteDC(cacheDC_);
        cacheDC_ = NULL;
        cacheBmp_ = NULL;
        cacheOldBmp_ = NULL;
    }
    cacheDC_ = CreateCompatibleDC(refDc);
    if (!cacheDC_) return false;
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* dibBits = NULL;
    cacheBmp_ = CreateDIBSection(refDc, &bmi, DIB_RGB_COLORS, &dibBits, NULL, 0);
    if (!cacheBmp_) {
        cacheBmp_ = CreateCompatibleBitmap(refDc, w, h);
    }
    cacheOldBmp_ = SelectObject(cacheDC_, cacheBmp_);
    cacheW_ = w;
    cacheH_ = h;
    RECT fullRc = { 0, 0, w, h };
    SetDCBrushColor(cacheDC_, RGB(0x0B, 0x0F, 0x17));
    FillRect(cacheDC_, &fullRc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    return true;
}

void ShellWindow::onPaint(HDC hdc, const RECT& dirtyRc) {
    // 1.4(b): In F11 Native Video Fullscreen, swapchain renders directly on top-level hwnd_!
    if (fullscreen_) {
        if (viewportHost_.isNativeVideoActive()) {
            viewportHost_.renderNativeFrame(true);
        } else {
            HBRUSH br = CreateSolidBrush(RGB(10, 13, 19));
            FillRect(hdc, &dirtyRc, br);
            DeleteObject(br);
        }
        return;
    }

    const int64_t tPaint0 = LatencyTelemetry::nowQpc();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    RECT paintRc = dirtyRc;
    if (paintRc.right <= paintRc.left || paintRc.bottom <= paintRc.top) {
        paintRc = rc;
    }

    if (ensurePaintCache(hdc, w, h)) {
        paintRc = rc;
    }

    auto hitsDirty = [&](const RECT& r) -> bool {
        RECT inter;
        return IntersectRect(&inter, &paintRc, &r) != FALSE;
    };

    const bool connected = bridge_->isConnected();
    const bool childCoversViewport = !showOnboarding_ && (viewportHost_.isNativeVideoActive() || viewportHost_.isMirrorEmbedded());

    double msHub = 0.0, msTop = 0.0, msDraw = 0.0, msRail = 0.0;
    TelemetrySummary vidTel = telemetry_.getSummary();
    {
        // Fast native GDI clear before GDI+ vector overlay (skip occluded viewport when native/embedded video active)
        HBRUSH bgBr = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        SetDCBrushColor(cacheDC_, RGB(0x0B, 0x0F, 0x17));
        if (!childCoversViewport) {
            FillRect(cacheDC_, &paintRc, bgBr);
        } else {
            RECT rClear;
            if (IntersectRect(&rClear, &paintRc, &topBarRc_)) FillRect(cacheDC_, &rClear, bgBr);
            if (IntersectRect(&rClear, &paintRc, &railRc_))   FillRect(cacheDC_, &rClear, bgBr);
            if (drawerOpen_ && IntersectRect(&rClear, &paintRc, &drawerRc_)) FillRect(cacheDC_, &rClear, bgBr);
        }

        Gdiplus::Graphics g(cacheDC_);
        g.SetClip(Gdiplus::Rect(paintRc.left, paintRc.top,
                                paintRc.right - paintRc.left, paintRc.bottom - paintRc.top));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        std::string devLabel;
        bool needDevLabel = (!fullscreen_ && (hitsDirty(topBarRc_) || (drawerOpen_ && hitsDirty(drawerRc_))));
        if (needDevLabel) {
            devLabel = bridge_->deviceName();
            if (connected) {
                char dbuf[112];
                std::snprintf(dbuf, sizeof(dbuf), "%s%s%s · %ux%u",
                              devLabel.empty() ? "Android" : devLabel.c_str(),
                              bridge_->isVirtualDisplayActive() ? " [2ND]" : "",
                              secureAppDetected_ ? " [SECURE APP]" : "",
                              lastPhoneW_, lastPhoneH_);
                devLabel = dbuf;
            }
        }

        // 1. Center Viewport backdrop / Onboarding Wizard / Device Hub (skip when child HWND owns viewportRc_!)
        if (!childCoversViewport && hitsDirty(viewportRc_)) {
            const int64_t ts = LatencyTelemetry::nowQpc();
            std::string qrSt;
            EnterCriticalSection(&auxLock_);
            qrSt = qrPairStatus_;
            LeaveCriticalSection(&auxLock_);
            if (showOnboarding_ && !demoMode_) {
                onboardingView_.paint(g, viewportRc_, toolsReady_, connected, (connecting_ != 0),
                                      (repairingTools_ != 0), lastConnectError_.empty() ? hubStatusMsg_ : lastConnectError_,
                                      hubStatusMsg_, adbDevices_, settings_.lastWirelessAddr,
                                      showingQrPair_, &qrMatrix_, qrSt, hoverX_, hoverY_);
            } else {
                HubViewState hub;
                buildHubViewState(hub);
                viewportHost_.paint(g, viewportRc_, connected, demoMode_, hub, hoverX_, hoverY_);
            }
            msHub = LatencyTelemetry::qpcToMs(LatencyTelemetry::nowQpc() - ts);
        }

        if (!fullscreen_) {
            // 2. TopBar (only when TopBar region is dirty)
            if (hitsDirty(topBarRc_)) {
                const int64_t ts = LatencyTelemetry::nowQpc();
                EngineTelemetry tel = engine_->getTelemetry();
                TopBarConnState tbConn = TB_CONN_IDLE;
                if (connected) tbConn = TB_CONN_CONNECTED;
                else if (demoMode_) tbConn = TB_CONN_DEMO;
                else if (reconnecting_) tbConn = TB_CONN_RECONNECTING;
                else if (!lastConnectError_.empty()) tbConn = TB_CONN_LOST;
                topBar_.paint(g, topBarRc_, devLabel, connected, demoMode_,
                              profile_, tel, settings_.alwaysOnTop,
                              (IsZoomed(hwnd_) != FALSE), hoverX_, hoverY_, vidTel, tbConn);
                if (connected && secureAppDetected_) {
                    RECT rSecBar = { topBarRc_.left + metrics_.dp(170), topBarRc_.bottom - metrics_.dp(16),
                                     topBarRc_.right - metrics_.dp(180), topBarRc_.bottom - metrics_.dp(1) };
                    if (rSecBar.right > rSecBar.left + metrics_.dp(120)) {
                        GdiPlusDraw::roundedCard(g, rSecBar, GdiPlusDraw::dpf(4.0f), Theme::ACCENT_AMBER_BG, Theme::ACCENT_AMBER, 1.0f);
                        GdiPlusDraw::textCenter(g, "This app blocks screen capture.",
                                                static_cast<float>(rSecBar.left + rSecBar.right) * 0.5f,
                                                static_cast<float>(rSecBar.top + rSecBar.bottom) * 0.5f,
                                                9.5f, true, Theme::ACCENT_AMBER);
                    }
                }
                msTop = LatencyTelemetry::qpcToMs(LatencyTelemetry::nowQpc() - ts);
            }

            // 3. Docked Outside-Viewport DrawerPanel (only when open and dirty)
            if (drawerOpen_ && hitsDirty(drawerRc_)) {
                const int64_t ts = LatencyTelemetry::nowQpc();
                ProbeResult pr;
                PhoneThermalInfo ti;
                std::string actEnc;
                std::vector<VideoEncoderInfo> encList;
                EnterCriticalSection(&auxLock_);
                pr = probeResult_;
                ti = thermalInfo_;
                actEnc = activeEncoderName_;
                encList = deviceEncoders_;
                LeaveCriticalSection(&auxLock_);
                drawerPanel_.setLatencyDiagnostics(vidTel, pr, viewportHost_.presentationModeLabel());
                drawerPanel_.setThermalAndEncoderInfo(bridge_->activeSerial(), ti, actEnc, encList);
                int pendingCnt = 0;
                if (connected && hasAppliedStreamSettings_) {
                    pendingCnt = AppSettings::countPendingStreamChanges(settings_, appliedStreamSettings_, bridge_->activeSerial());
                }
                drawerPanel_.setPendingStreamChangeCount(pendingCnt);
                int slideDx = animator_.isActive(ANIM_DRAWER_SLIDE)
                    ? static_cast<int>(animator_.value(ANIM_DRAWER_SLIDE, 0.0f) + 0.5f) : 0;
                std::vector<std::string> logs = bridge_->getRecentLogs();
                drawerPanel_.paint(g, drawerRc_, profile_, settings_, profilePath_,
                                   hudOverlay_.selectedBtnIndex(),
                                   (hudOverlay_.rebindTarget() == HUD_REBIND_BUTTON),
                                   connected, devLabel, activeAppPackage_, logs, hoverX_, hoverY_, slideDx);
                msDraw = LatencyTelemetry::qpcToMs(LatencyTelemetry::nowQpc() - ts);
            }

            // 4. Right Emulator SideRail (only when dirty)
            if (hitsDirty(railRc_)) {
                const int64_t ts = LatencyTelemetry::nowQpc();
                SideRailState st;
                st.aimMode = aimMode_;
                st.drawerOpen = drawerOpen_;
                st.activeDrawerTab = drawerPanel_.activeTab();
                st.showHud = profile_.showHudInPlay;
                st.fullscreen = fullscreen_;
                st.connected = connected;
                sideRail_.paint(g, railRc_, st, hoverX_, hoverY_);
                msRail = LatencyTelemetry::qpcToMs(LatencyTelemetry::nowQpc() - ts);
            }

            // S6: Debug overlay strip (Ctrl+Shift+D) over top edge of viewport/TopBar
            if (debugOverlayVisible_ && hitsDirty(topBarRc_)) {
                char dbg[160];
                std::snprintf(dbg, sizeof(dbg),
                              "DBG %s | rx->pres %.1f/%.1fms dec/prs/skp=%llu/%llu/%llu | UI max=%.2fms stalls=%u | paint=%.2fms",
                              viewportHost_.presentationModeLabel().c_str(),
                              vidTel.rxToPresentP50Ms, vidTel.rxToPresentP95Ms,
                              static_cast<unsigned long long>(vidTel.decodedCount),
                              static_cast<unsigned long long>(vidTel.presentedCount),
                              static_cast<unsigned long long>(vidTel.skippedCount),
                              maxUiMsgMs_, uiStallCount_, lastPaintMs_);
                RECT rDbg = { topBarRc_.left + 8, topBarRc_.bottom - 14, topBarRc_.right - 8, topBarRc_.bottom };
                GdiPlusDraw::fillRect(g, rDbg, 0xD8081018u);
                GdiPlusDraw::textLeft(g, dbg, static_cast<float>(rDbg.left + 4),
                                      static_cast<float>(rDbg.top + 7), 9.5f, true, Theme::ACCENT_CYAN);
            }

            // N29 + X9 + U4: Render shell-layer toast/snackbar when no phone video/HUD layer is active
            if (!childCoversViewport && !shellToastMsg_.empty() &&
                static_cast<LONG>(GetTickCount() - shellToastExpireTick_) < 0 &&
                hitsDirty(viewportRc_)) {
                int vw = viewportRc_.right - viewportRc_.left;
                int btnW = shellToastActionLabel_.empty()
                    ? 0
                    : (metrics_.dp(24) + static_cast<int>(metrics_.dpf(static_cast<float>(shellToastActionLabel_.size()) * 7.0f)));
                int pw = metrics_.dp(42) + static_cast<int>(metrics_.dpf(static_cast<float>(shellToastMsg_.size()) * 6.8f)) + (btnW > 0 ? btnW + metrics_.dp(10) : 0);
                if (pw < metrics_.dp(156)) pw = metrics_.dp(156);
                if (pw > vw - metrics_.dp(28)) pw = vw - metrics_.dp(28);
                int ph = metrics_.dp(32);
                int cx = (viewportRc_.left + viewportRc_.right) / 2;
                int cy = viewportRc_.bottom - ph / 2 - metrics_.dp(16);
                RECT tr = { cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2 };
                uint32_t kindCol = (shellToastKind_ == TOAST_SUCCESS) ? Theme::SEM_SUCCESS
                                 : (shellToastKind_ == TOAST_WARNING) ? Theme::SEM_WARNING
                                 : (shellToastKind_ == TOAST_ERROR)   ? Theme::SEM_DANGER
                                                                      : Theme::SEM_INFO;
                GdiPlusDraw::roundedCard(g, tr, metrics_.dpf(14.0f), 0xE2101622u, kindCol, 1.2f);
                Gdiplus::SolidBrush dotBr(GdiPlusDraw::argb(kindCol));
                float dR = metrics_.dpf(3.5f);
                g.FillEllipse(&dotBr, static_cast<float>(tr.left + metrics_.dp(10)), static_cast<float>(cy) - dR, dR * 2.0f, dR * 2.0f);
                if (btnW > 0) {
                    int msgW = pw - btnW - metrics_.dp(30);
                    GdiPlusDraw::textLeft(g, shellToastMsg_, static_cast<float>(tr.left + metrics_.dp(22)),
                                          static_cast<float>(cy), 11.5f, true, Theme::TEXT_PRIMARY, static_cast<float>(msgW));
                    RECT actRc = { tr.right - btnW - metrics_.dp(6), tr.top + metrics_.dp(4),
                                   tr.right - metrics_.dp(6), tr.bottom - metrics_.dp(4) };
                    shellToastActionRc_ = actRc;
                    GdiPlusDraw::roundedCard(g, actRc, metrics_.dpf(9.0f), Theme::ACCENT_CYAN_BG, kindCol, 1.1f);
                    GdiPlusDraw::textCenter(g, shellToastActionLabel_,
                                            static_cast<float>(actRc.left + actRc.right) * 0.5f,
                                            static_cast<float>(actRc.top + actRc.bottom) * 0.5f,
                                            11.0f, true, kindCol);
                } else {
                    ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
                    GdiPlusDraw::textCenter(g, shellToastMsg_, static_cast<float>(cx + metrics_.dp(4)), static_cast<float>(cy),
                                            11.5f, true, Theme::TEXT_PRIMARY);
                }
            } else if (!shellToastWasVisible_) {
                ZeroMemory(&shellToastActionRc_, sizeof(shellToastActionRc_));
            }

            // Crisp outer 1px window border when restored (N35: skip when DWM Win11 rounding is active)
            if (!IsZoomed(hwnd_) && !dwmRoundedActive_) {
                GdiPlusDraw::strokeRoundedRect(g, 0.5f, 0.5f, static_cast<float>(w - 1), static_cast<float>(h - 1), 0.0f, Theme::BORDER_STRONG, 1.0f);
            }
        }
    }

    if (!childCoversViewport) {
        int bx = paintRc.left;
        int by = paintRc.top;
        int bw = paintRc.right - paintRc.left;
        int bh = paintRc.bottom - paintRc.top;
        BitBlt(hdc, bx, by, bw, bh, cacheDC_, bx, by, SRCCOPY);
    } else {
        auto blitSub = [&](const RECT& sub) {
            RECT rBlt;
            if (IntersectRect(&rBlt, &paintRc, &sub)) {
                BitBlt(hdc, rBlt.left, rBlt.top,
                       rBlt.right - rBlt.left, rBlt.bottom - rBlt.top,
                       cacheDC_, rBlt.left, rBlt.top, SRCCOPY);
            }
        };
        blitSub(topBarRc_);
        blitSub(railRc_);
        if (drawerOpen_) blitSub(drawerRc_);
    }
    lastPaintMs_ = LatencyTelemetry::qpcToMs(LatencyTelemetry::nowQpc() - tPaint0);
    if (lastPaintMs_ > 16.0 || (consoleEnabled_ && lastPaintMs_ > 8.0)) {
        char pbuf[160];
        std::snprintf(pbuf, sizeof(pbuf),
                      "[S6-Paint] total=%.2fms (hub=%.2f top=%.2f drawer=%.2f rail=%.2f)",
                      lastPaintMs_, msHub, msTop, msDraw, msRail);
        if (lastPaintMs_ > 16.0) {
            bridge_->appendLog(pbuf);
        }
        if (consoleEnabled_) {
            std::printf("%s\n", pbuf);
        }
    }
}

// ============================================================================
// Sprint 3B: Animator, Layered Tooltips, X5 Command Palette, X6 Cheat Sheet,
//            X7 Searchable Profile Dropdown & Auto-Switch Transparency
// ============================================================================

void ShellWindow::syncAnimTimer() {
    if (!hwnd_) return;
    bool need = animator_.hasActive();
    if (need && !animTimerActive_) {
        SetTimer(hwnd_, TIMER_ANIM, 8, NULL);
        animTimerActive_ = true;
    } else if (!need && animTimerActive_) {
        KillTimer(hwnd_, TIMER_ANIM);
        animTimerActive_ = false;
    }
}

void ShellWindow::updateTooltipAtPoint(int mx, int my) {
    if (!hwnd_ || fullscreen_ || activePopup_ != POPUP_NONE) {
        tooltipPopup_.hide();
        return;
    }
    if (ptIn(railRc_, mx, my)) {
        std::string tip = sideRail_.tooltipAt(mx, my);
        RECT r = { 0, 0, 0, 0 };
        if (!tip.empty() && sideRail_.itemRectAt(mx, my, r)) {
            POINT pt = { r.left, (r.top + r.bottom) / 2 };
            ClientToScreen(hwnd_, &pt);
            tooltipPopup_.showAt(pt.x, pt.y, tip, true);
            return;
        }
    } else if (ptIn(topBarRc_, mx, my)) {
        std::string tip = topBar_.tooltipAt(mx, my);
        if (!tip.empty()) {
            int dummyId = 0;
            RECT r = { mx - 20, topBarRc_.top, mx + 20, topBarRc_.bottom };
            topBar_.hoverItemAt(mx, my, dummyId, r);
            POINT pt = { (r.left + r.right) / 2, topBarRc_.bottom + metrics_.dp(4) };
            ClientToScreen(hwnd_, &pt);
            tooltipPopup_.showAt(pt.x, pt.y, tip, false);
            return;
        }
    }
    tooltipPopup_.hide();
}

void ShellWindow::recordRecentProfile(const std::string& path) {
    if (path.empty()) return;
    for (std::vector<std::string>::iterator it = recentProfilePaths_.begin();
         it != recentProfilePaths_.end(); ++it) {
        if (lstrcmpiA(it->c_str(), path.c_str()) == 0) {
            recentProfilePaths_.erase(it);
            break;
        }
    }
    recentProfilePaths_.insert(recentProfilePaths_.begin(), path);
    if (recentProfilePaths_.size() > 6) {
        recentProfilePaths_.resize(6);
    }
}

void ShellWindow::rebuildProfileMenuItems() {
    profileMenuItems_.clear();
    std::vector<std::string> files = ProfileStore::listProfiles(profilesDir_);
    for (size_t i = 0; i < files.size(); ++i) {
        ProfileMenuItem item;
        item.path = files[i];
        size_t sl = files[i].find_last_of("\\/");
        std::string base = (sl == std::string::npos) ? files[i] : files[i].substr(sl + 1);
        Profile tmp;
        if (ProfileStore::load(files[i], tmp)) {
            item.displayName = tmp.name.empty() ? base : tmp.name;
            item.appPackage = tmp.appPackage;
        } else {
            item.displayName = base;
        }
        item.active = (lstrcmpiA(files[i].c_str(), profilePath_.c_str()) == 0);
        for (size_t ri = 0; ri < recentProfilePaths_.size(); ++ri) {
            if (lstrcmpiA(recentProfilePaths_[ri].c_str(), files[i].c_str()) == 0) {
                item.recent = true;
                break;
            }
        }
        profileMenuItems_.push_back(item);
    }
}

void ShellWindow::recheckAutoSwitchForCurrentApp() {
    if (!settings_.autoSwitchProfiles || drawerOpen_ || editMode_ || demoMode_) return;
    if (activeAppPackage_.empty()) return;
    handleForegroundAppChanged(activeAppPackage_);
}

void ShellWindow::initCommandPalette() {
    baseCommands_.clear();
    auto addCmd = [&](const char* id, const char* title, const char* kw, const char* hk, const char* cat) {
        CommandEntry e;
        e.id = id;
        e.title = title;
        e.keywords = kw;
        e.hotkey = hk;
        e.category = cat;
        baseCommands_.push_back(e);
    };
    addCmd("aim.toggle",      "Toggle Aim Lock / Cursor Mode",     "aim lock cursor camera mouse", "F1",          "Play");
    addCmd("drawer.keys",     "Open Keymap Studio",                "keys editor palette add bind", "F2",          "Studio");
    addCmd("drawer.sens",     "Open Sensitivity & Aim Physics",    "sens dpi accel fire joy",      "",            "Studio");
    addCmd("drawer.profiles", "Open Profiles Manager",             "profile config template import","",            "Profiles");
    addCmd("drawer.device",   "Open Device & Stream Settings",     "device stream fps bitrate adb","",            "Device");
    addCmd("tab.gamepad",     "Open Gamepad & Controller Tab",     "gamepad pad controller uhid",   "",            "Gamepad");
    addCmd("pad.mode",        "Switch Gamepad Touch / Native UHID","gamepad uhid native touch mode","",            "Gamepad");
    addCmd("fire.cycle",      "Cycle Fire Mode",                   "fire drag claw dual",          "F3",          "Play");
    addCmd("hud.toggle",      "Toggle HUD Overlay Badges",         "hud hide show opacity",        "F4",          "Play");
    addCmd("touch.reset",     "Reset All Virtual Touches",         "reset unstick release",        "F5",          "Play");
    addCmd("win.fullscreen",  "Toggle Fullscreen Mode",            "fullscreen max borderless",    "F11",         "View");
    addCmd("win.pin",         "Toggle Window Always on Top",       "pin topmost float",            "",            "View");
    addCmd("cheatsheet.open", "Open Live Keybinding Cheat Sheet",  "help hotkeys bindings list",   "Ctrl+/",      "Help");
    addCmd("preset.comp",     "Stream Preset: Competitive",        "preset competitive latency",   "",            "Stream");
    addCmd("preset.bal",      "Stream Preset: Balanced",           "preset balanced",              "",            "Stream");
    addCmd("preset.qual",     "Stream Preset: Quality",            "preset quality native",        "",            "Stream");
    addCmd("preset.wifi",     "Stream Preset: Wi-Fi",              "preset wifi wireless",         "",            "Stream");
    addCmd("setting.audio",   "Toggle Phone Audio Forwarding",     "audio sound scrcpy",           "",            "Stream");
    addCmd("setting.clipboard","Toggle Clipboard Auto-Sync",       "clipboard copy paste sync winv","",           "Privacy");
    addCmd("lang.cycle",      "Switch UI Language",              "language locale i18n en pt es id hi ru", "",   "Display");
    addCmd("setting.screenoff","Toggle Turn Phone Screen Off",     "screen off power battery",     "",            "Device");
    addCmd("setting.autoswitch","Toggle Per-App Auto-Switch",      "auto switch package app",      "",            "Input");
    addCmd("setting.gamepad", "Toggle XInput Gamepad-to-Touch",    "gamepad controller xbox",      "",            "Gamepad");
    addCmd("diag.debug",      "Toggle Latency & UI Debug Strip",   "debug overlay stats s6",       "Ctrl+Shift+D","Debug");
    addCmd("diag.probe",      "Run Round-Trip Latency Probe",      "probe latency benchmark",      "Ctrl+Shift+L","Debug");
    addCmd("app.screenshot",  "Capture Phone Screenshot",          "screenshot png camera",        "",            "Device");
    addCmd("app.reconnect",   "Apply Settings & Reconnect Stream", "reconnect restart stream",     "",            "Device");
    addCmd("app.onboarding",  "Open Setup Guide / Onboarding",     "onboarding guide wizard help", "",            "Help");
}

void ShellWindow::openPopup(int modeInt) {
    if (!hwnd_) return;
    ActivePopupMode mode = static_cast<ActivePopupMode>(modeInt);
    if (mode == POPUP_NONE) {
        closePopup();
        return;
    }
    if (aimMode_) setAimMode(false);
    tooltipPopup_.hide();
    drawerPanel_.clearFocus();
    editingHubWireless_ = false;

    activePopup_ = mode;
    popupQuery_.clear();
    popupSelectedRow_ = 0;
    popupHoverRow_ = -1;
    popupRowRects_.clear();
    ZeroMemory(&popupManageProfilesRc_, sizeof(popupManageProfilesRc_));

    RECT cr;
    GetClientRect(hwnd_, &cr);
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;

    RECT localRc = { 0, 0, 320, 240 };
    if (mode == POPUP_CMD_PALETTE) {
        std::vector<CommandEntry> all = baseCommands_;
        rebuildProfileMenuItems();
        for (size_t i = 0; i < profileMenuItems_.size(); ++i) {
            CommandEntry pe;
            pe.id = "profile:" + profileMenuItems_[i].path;
            pe.title = "Switch Profile: " + profileMenuItems_[i].displayName;
            pe.keywords = profileMenuItems_[i].appPackage + " profile switch config";
            pe.hotkey = profileMenuItems_[i].active ? "Active" : "";
            pe.category = "Profile";
            all.push_back(pe);
        }
        cmdPalette_.setCommands(all);
        popupFiltered_ = cmdPalette_.filter("");

        int pw = metrics_.dp(540);
        if (pw > cw - metrics_.dp(32)) pw = cw - metrics_.dp(32);
        int ph = metrics_.dp(390);
        if (ph > ch - metrics_.dp(80)) ph = ch - metrics_.dp(80);
        int x = (cw - pw) / 2;
        int y = (fullscreen_ ? 0 : topBarRc_.bottom) + metrics_.dp(22);
        localRc = { x, y, x + pw, y + ph };
    } else if (mode == POPUP_CHEAT_SHEET) {
        cheatSheetData_ = CheatSheetModel::build(profile_);
        int pw = metrics_.dp(720);
        if (pw > cw - metrics_.dp(28)) pw = cw - metrics_.dp(28);
        int ph = metrics_.dp(450);
        if (ph > ch - metrics_.dp(64)) ph = ch - metrics_.dp(64);
        int x = (cw - pw) / 2;
        int y = (ch - ph) / 2;
        localRc = { x, y, x + pw, y + ph };
    } else if (mode == POPUP_PROFILE_DROPDOWN) {
        rebuildProfileMenuItems();
        popupFiltered_ = ProfileDropdownModel::filter(profileMenuItems_, "");
        int rows = static_cast<int>(popupFiltered_.size());
        if (rows < 1) rows = 1;
        if (rows > 7) rows = 7;
        int pw = metrics_.dp(320);
        int ph = metrics_.dp(90) + rows * metrics_.dp(40);
        if (ph > ch - metrics_.dp(70)) ph = ch - metrics_.dp(70);
        RECT profRc = topBar_.profilePillRect();
        int x = (profRc.right > profRc.left) ? profRc.left : metrics_.dp(280);
        int y = topBarRc_.bottom + metrics_.dp(4);
        localRc = { x, y, x + pw, y + ph };
    }

    POINT tl = { localRc.left, localRc.top };
    ClientToScreen(hwnd_, &tl);
    RECT scrRc = { tl.x, tl.y, tl.x + (localRc.right - localRc.left), tl.y + (localRc.bottom - localRc.top) };
    modalPopup_.showBounds(scrRc);
    engine_->setShellMenuOpen(true);
    syncInputHookSnapshot();
}

void ShellWindow::closePopup() {
    if (activePopup_ == POPUP_NONE && !modalPopup_.isVisible()) return;
    activePopup_ = POPUP_NONE;
    popupQuery_.clear();
    popupFiltered_.clear();
    popupRowRects_.clear();
    modalPopup_.hide();
    engine_->setShellMenuOpen(drawerOpen_);
    syncInputHookSnapshot();
}

void ShellWindow::onPadNav(void* ctx, uint8_t navMask) {
    ShellWindow* self = reinterpret_cast<ShellWindow*>(ctx);
    if (self && self->hwnd_ && navMask != 0) {
        PostMessageW(self->hwnd_, MSG_PAD_NAV, static_cast<WPARAM>(navMask), 0);
    }
}

void ShellWindow::executeCommandById(const std::string& cmdId) {
    closePopup();
    if (cmdId.compare(0, 8, "profile:") == 0) {
        std::string target = cmdId.substr(8);
        DrawerActionResult res;
        res.requestSwitchProfile = true;
        res.switchProfilePath = target;
        handleDrawerResult(res);
        return;
    }
    if (cmdId == "aim.toggle")      { setAimMode(!aimMode_); return; }
    if (cmdId == "drawer.keys")     { setDrawerState(true, 0); return; }
    if (cmdId == "drawer.sens")     { setDrawerState(true, 1); return; }
    if (cmdId == "drawer.profiles") { setDrawerState(true, 2); return; }
    if (cmdId == "drawer.device")   { setDrawerState(true, 3); return; }
    if (cmdId == "tab.gamepad")     { setDrawerState(true, 4); return; }
    if (cmdId == "pad.mode") {
        profile_.padMode = (profile_.padMode == PAD_MODE_NATIVE) ? PAD_MODE_TOUCH : PAD_MODE_NATIVE;
        applyProfileToEngine(true);
        invalidateShell();
        showToast(profile_.padMode == PAD_MODE_NATIVE ? "Controller: NATIVE UHID" : "Controller: TOUCH MAP", 1800, TOAST_INFO);
        return;
    }
    if (cmdId == "fire.cycle")      { toggleFireMode(); return; }
    if (cmdId == "hud.toggle")      { toggleHud(); return; }
    if (cmdId == "touch.reset")     { triggerQuickReset(); return; }
    if (cmdId == "win.fullscreen")  { toggleFullscreen(); return; }
    if (cmdId == "win.pin")         { toggleAlwaysOnTop(); return; }
    if (cmdId == "cheatsheet.open") { openPopup(POPUP_CHEAT_SHEET); return; }
    if (cmdId == "preset.comp" || cmdId == "preset.bal" || cmdId == "preset.qual" || cmdId == "preset.wifi") {
        StreamPresetId pid = (cmdId == "preset.comp") ? PRESET_COMPETITIVE
                           : (cmdId == "preset.bal")  ? PRESET_BALANCED
                           : (cmdId == "preset.qual") ? PRESET_QUALITY : PRESET_WIFI;
        settings_.applyStreamPreset(pid);
        scheduleSettingsSave();
        invalidateShell();
        showToast(std::string("Preset: ") + AppSettings::streamPresetName(pid) + " (Apply & Reconnect to use)", 2400, TOAST_INFO);
        return;
    }
    if (cmdId == "setting.audio") {
        settings_.mirrorAudio = !settings_.mirrorAudio;
        scheduleSettingsSave();
        invalidateShell();
        showToast(settings_.mirrorAudio ? "Phone Audio Forwarding: ON" : "Phone Audio Forwarding: OFF", 1800, TOAST_INFO);
        return;
    }
    if (cmdId == "setting.clipboard") {
        settings_.clipboardSync = !settings_.clipboardSync;
        scheduleSettingsSave();
        invalidateShell();
        showToast(settings_.clipboardSync ? "Clipboard Sync: ON (Apply & Reconnect)" : "Clipboard Sync: OFF (Apply & Reconnect)", 2200, TOAST_INFO);
        return;
    }
    if (cmdId == "lang.cycle") {
        const std::vector<LanguageInfo>& langs = Strings::languages();
        int idx = (Strings::languageIndex(settings_.uiLanguage) + 1) % static_cast<int>(langs.size());
        settings_.uiLanguage = langs[idx].code;
        Strings::setLanguage(settings_.uiLanguage);
        scheduleSettingsSave();
        invalidateShell();
        showToast(std::string("Language: ") + langs[idx].nativeName, 1800, TOAST_INFO);
        return;
    }
    if (cmdId == "setting.screenoff") {
        settings_.turnScreenOff = !settings_.turnScreenOff;
        scheduleSettingsSave();
        lastAppliedScreenOff_ = settings_.turnScreenOff;
        if (bridge_->isConnected() && !bridge_->isVirtualDisplayActive()) {
            bridge_->setDisplayPower(!settings_.turnScreenOff);
        }
        invalidateShell();
        showToast(settings_.turnScreenOff ? "Phone Screen: OFF" : "Phone Screen: ON", 1600, TOAST_INFO);
        return;
    }
    if (cmdId == "setting.autoswitch") {
        settings_.autoSwitchProfiles = !settings_.autoSwitchProfiles;
        InterlockedExchange(&autoSwitchAtomic_, settings_.autoSwitchProfiles ? 1 : 0);
        scheduleSettingsSave();
        invalidateShell();
        showToast(settings_.autoSwitchProfiles ? "Per-App Auto-Switch: ON" : "Per-App Auto-Switch: OFF", 1600, TOAST_INFO);
        return;
    }
    if (cmdId == "setting.gamepad") {
        settings_.gamepadEnabled = !settings_.gamepadEnabled;
        engine_->setGamepadEnabled(settings_.gamepadEnabled);
        scheduleSettingsSave();
        invalidateShell();
        showToast(settings_.gamepadEnabled ? "XInput Gamepad: ON" : "XInput Gamepad: OFF", 1600, TOAST_INFO);
        return;
    }
    if (cmdId == "diag.debug") {
        debugOverlayVisible_ = !debugOverlayVisible_;
        lastTopBarSig_.valid = false;
        invalidateShell();
        showToast(debugOverlayVisible_ ? "Debug Overlay: ON" : "Debug Overlay: OFF", 1200, TOAST_INFO);
        return;
    }
    if (cmdId == "diag.probe") {
        startRoundTripProbe(autoProbeSamples_);
        return;
    }
    if (cmdId == "app.screenshot") { takeScreenshot(); return; }
    if (cmdId == "app.reconnect") {
        scheduleSettingsSave();
        if (bridge_->isConnected()) startAsyncConnect(bridge_->activeSerial());
        return;
    }
    if (cmdId == "app.onboarding") {
        if (drawerOpen_) setDrawerState(false, drawerPanel_.activeTab());
        showOnboarding_ = true;
        onboardingView_.setStep(toolsReady_ ? OB_STEP_PREPARE : OB_STEP_TOOLS);
        syncHudAndVideo();
        invalidateShell();
    }
}

void ShellWindow::handlePopupKeyDown(int vk) {
    if (vk == VK_ESCAPE) {
        closePopup();
        return;
    }
    if (activePopup_ == POPUP_CHEAT_SHEET) {
        return;
    }
    int count = static_cast<int>(popupFiltered_.size());
    if (vk == VK_DOWN && count > 0) {
        popupSelectedRow_ = (popupSelectedRow_ + 1) % count;
        modalPopup_.repaint();
        return;
    }
    if (vk == VK_UP && count > 0) {
        popupSelectedRow_ = (popupSelectedRow_ + count - 1) % count;
        modalPopup_.repaint();
        return;
    }
    if (vk == VK_BACK && !popupQuery_.empty()) {
        popupQuery_.pop_back();
        if (activePopup_ == POPUP_CMD_PALETTE) {
            popupFiltered_ = cmdPalette_.filter(popupQuery_);
        } else if (activePopup_ == POPUP_PROFILE_DROPDOWN) {
            popupFiltered_ = ProfileDropdownModel::filter(profileMenuItems_, popupQuery_);
        }
        popupSelectedRow_ = 0;
        modalPopup_.repaint();
        return;
    }
    if (vk == VK_RETURN && popupSelectedRow_ >= 0 && popupSelectedRow_ < count) {
        int rawIdx = popupFiltered_[popupSelectedRow_];
        if (activePopup_ == POPUP_CMD_PALETTE) {
            executeCommandById(cmdPalette_.commands()[rawIdx].id);
        } else if (activePopup_ == POPUP_PROFILE_DROPDOWN) {
            std::string target = profileMenuItems_[rawIdx].path;
            closePopup();
            DrawerActionResult res;
            res.requestSwitchProfile = true;
            res.switchProfilePath = target;
            handleDrawerResult(res);
        }
    }
}

void ShellWindow::handlePopupChar(wchar_t ch) {
    if (activePopup_ != POPUP_CMD_PALETTE && activePopup_ != POPUP_PROFILE_DROPDOWN) return;
    if (ch < 32 || ch > 126) return;
    if (popupQuery_.size() >= 32) return;
    popupQuery_.push_back(static_cast<char>(ch));
    if (activePopup_ == POPUP_CMD_PALETTE) {
        popupFiltered_ = cmdPalette_.filter(popupQuery_);
    } else {
        popupFiltered_ = ProfileDropdownModel::filter(profileMenuItems_, popupQuery_);
    }
    popupSelectedRow_ = 0;
    modalPopup_.repaint();
}

void ShellWindow::onPopupPaint(Gdiplus::Graphics& g, int w, int h) {
    if (activePopup_ == POPUP_CMD_PALETTE) {
        cmdPalette_.paint(g, w, h, popupQuery_, popupFiltered_, popupSelectedRow_, popupHoverRow_, popupRowRects_);
    } else if (activePopup_ == POPUP_CHEAT_SHEET) {
        CheatSheetModel::paint(g, w, h, cheatSheetData_);
    } else if (activePopup_ == POPUP_PROFILE_DROPDOWN) {
        ProfileDropdownModel::paint(g, w, h, popupQuery_, profileMenuItems_, popupFiltered_,
                                    popupSelectedRow_, popupHoverRow_, popupRowRects_, popupManageProfilesRc_);
    }
}

void ShellWindow::onPopupHover(int x, int y) {
    int newHov = -1;
    for (size_t i = 0; i < popupRowRects_.size(); ++i) {
        if (ptIn(popupRowRects_[i], x, y)) {
            newHov = static_cast<int>(i);
            break;
        }
    }
    if (newHov != popupHoverRow_) {
        popupHoverRow_ = newHov;
        modalPopup_.repaint();
    }
}

void ShellWindow::onPopupClick(int x, int y) {
    if (activePopup_ == POPUP_CHEAT_SHEET) {
        closePopup();
        return;
    }
    if (activePopup_ == POPUP_PROFILE_DROPDOWN && ptIn(popupManageProfilesRc_, x, y)) {
        closePopup();
        setDrawerState(true, 2);
        return;
    }
    for (size_t i = 0; i < popupRowRects_.size(); ++i) {
        if (ptIn(popupRowRects_[i], x, y) && i < popupFiltered_.size()) {
            int rawIdx = popupFiltered_[i];
            if (activePopup_ == POPUP_CMD_PALETTE) {
                executeCommandById(cmdPalette_.commands()[rawIdx].id);
            } else if (activePopup_ == POPUP_PROFILE_DROPDOWN) {
                std::string target = profileMenuItems_[rawIdx].path;
                closePopup();
                DrawerActionResult res;
                res.requestSwitchProfile = true;
                res.switchProfilePath = target;
                handleDrawerResult(res);
            }
            return;
        }
    }
}

void ShellWindow::onPopupWheel(int delta) {
    int count = static_cast<int>(popupFiltered_.size());
    if (count <= 0) return;
    popupSelectedRow_ = (delta < 0)
        ? ((popupSelectedRow_ + 1) % count)
        : ((popupSelectedRow_ + count - 1) % count);
    modalPopup_.repaint();
}
