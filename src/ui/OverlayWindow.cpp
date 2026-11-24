#include "ui/OverlayWindow.hpp"

#include "config/KeyCodes.hpp"
#include "config/ProfileStore.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

static OverlayWindow* g_overlayInstance = NULL;

static const COLORREF COLOR_KEY_TRANSPARENT = RGB(255, 0, 255);
static const UINT_PTR TIMER_SYNC_REPAINT = 1;

LRESULT CALLBACK globalMouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_overlayInstance) {
        return g_overlayInstance->handleLowLevelMouse(nCode, wParam, lParam);
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT CALLBACK globalKeyHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (g_overlayInstance) {
        return g_overlayInstance->handleLowLevelKeyboard(nCode, wParam, lParam);
    }
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

} // namespace

OverlayWindow::OverlayWindow(ScrcpyBridge* bridge, InputEngine* engine,
                             const std::string& profilesDir, const std::string& profilePath,
                             const std::string& mirrorTitle, bool demoMode)
    : bridge_(bridge), engine_(engine), profilesDir_(profilesDir),
      profilePath_(profilePath), mirrorTitle_(mirrorTitle), demoMode_(demoMode) {
    ZeroMemory(&mirrorClientScreenRect_, sizeof(mirrorClientScreenRect_));
    ZeroMemory(&floatBallPressPt_, sizeof(floatBallPressPt_));
    std::memset(hookKeyDown_, 0, sizeof(hookKeyDown_));
    profile_ = engine_->getProfileCopy();
    studioPanel_.setProfilesDir(profilesDir_, profilePath_);
}

OverlayWindow::~OverlayWindow() {
    setAimMode(false);
    removeHooks();
    if (cacheDC_) {
        if (cacheOldBmp_) SelectObject(cacheDC_, cacheOldBmp_);
        if (cacheBmp_) DeleteObject(cacheBmp_);
        DeleteDC(cacheDC_);
        cacheDC_ = NULL;
        cacheBmp_ = NULL;
        cacheOldBmp_ = NULL;
    }
    if (fonts_.smallFont) DeleteObject(fonts_.smallFont);
    if (fonts_.boldFont)  DeleteObject(fonts_.boldFont);
    if (fonts_.titleFont) DeleteObject(fonts_.titleFont);
    if (g_overlayInstance == this) g_overlayInstance = NULL;
}

bool OverlayWindow::create(HINSTANCE hInst) {
    hInst_ = hInst;
    g_overlayInstance = this;

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = OverlayWindow::wndProc;
    wc.hInstance = hInst_;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "PhantomTouchOverlayClass";

    if (!RegisterClassExA(&wc)) {
        return false;
    }

    fonts_.smallFont = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    fonts_.boldFont  = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    fonts_.titleFont = CreateFontA(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    if (demoMode_) {
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        RECT rc = {0, 0, 1280, 720};
        AdjustWindowRect(&rc, style, FALSE);
        hwnd_ = CreateWindowExA(0, wc.lpszClassName,
                                "Phantom Touch Studio [DEMO / OFFLINE EDITOR]",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, hInst_, this);
    } else {
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
        hwnd_ = CreateWindowExA(exStyle, wc.lpszClassName,
                                "Phantom Touch Overlay",
                                WS_POPUP,
                                100, 100, 960, 540,
                                NULL, NULL, hInst_, this);
        applyHudOpacity();
    }

    if (!hwnd_) return false;

    registerRawInput();
    installHooks();
    SetTimer(hwnd_, TIMER_SYNC_REPAINT, 16, NULL);

    if (demoMode_) {
        showToast("Demo Mode: Click the floating 'CFG' ball (or F2) to open Keymap Studio!", 5000);
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    } else {
        showToast("Connected! Click floating 'CFG' ball (or F2) for Studio | " + KeyCodes::toName(profile_.toggleAimVk) + " or F1 for Aim Mode", 4500);
    }
    return true;
}

void OverlayWindow::applyHudOpacity() {
    if (demoMode_ || !hwnd_ || editMode_) return;
    BYTE alpha = static_cast<BYTE>(clampRange(profile_.hudOpacity, 0.15f, 1.00f) * 255.0f);
    SetLayeredWindowAttributes(hwnd_, COLOR_KEY_TRANSPARENT, alpha, LWA_COLORKEY | LWA_ALPHA);
}

void OverlayWindow::registerRawInput() {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void OverlayWindow::installHooks() {
    mouseHook_ = SetWindowsHookExA(WH_MOUSE_LL, globalMouseHookProc, hInst_, 0);
    keyHook_   = SetWindowsHookExA(WH_KEYBOARD_LL, globalKeyHookProc, hInst_, 0);
}

void OverlayWindow::removeHooks() {
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = NULL;
    }
    if (keyHook_) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = NULL;
    }
}

int OverlayWindow::runMessageLoop() {
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void OverlayWindow::showToast(const std::string& msg, DWORD durationMs) {
    toastMsg_ = msg;
    toastExpireTick_ = GetTickCount() + durationMs;
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayWindow::applyProfileToEngine() {
    engine_->setProfile(profile_);
    applyHudOpacity();
}

void OverlayWindow::saveCurrentProfile() {
    if (ProfileStore::save(profilePath_, profile_)) {
        ProfileStore::saveLastActive(profilesDir_, profilePath_);
        studioPanel_.refreshProfileList();
        showToast("Saved config -> " + profilePath_, 2200);
    } else {
        showToast("ERROR: Could not write " + profilePath_, 3000);
    }
}

void OverlayWindow::setAimMode(bool enabled) {
    if (editMode_ && enabled) {
        setEditMode(false);
    }
    if (aimMode_ == enabled) return;

    aimMode_ = enabled;
    engine_->setAimMode(enabled);
    cursorDirectDragging_ = false;
    floatBallHookPressed_ = false;

    if (aimMode_) {
        if (!demoMode_ && hwnd_) {
            LONG ex = GetWindowLongA(hwnd_, GWL_EXSTYLE);
            ex &= ~WS_EX_TRANSPARENT;
            SetWindowLongA(hwnd_, GWL_EXSTYLE, ex);
            SetForegroundWindow(hwnd_);
        }
        updateClipCursor();
        if (!cursorHidden_) {
            ShowCursor(FALSE);
            cursorHidden_ = true;
        }
        SetCursor(NULL);
        showToast("AIM MODE ON  (Press " + KeyCodes::toName(profile_.toggleAimVk) + " or F1 to release mouse)", 1400);
    } else {
        ClipCursor(NULL);
        if (cursorHidden_) {
            ShowCursor(TRUE);
            cursorHidden_ = false;
        }
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        if (!demoMode_ && hwnd_ && !editMode_) {
            LONG ex = GetWindowLongA(hwnd_, GWL_EXSTYLE);
            ex |= WS_EX_TRANSPARENT;
            SetWindowLongA(hwnd_, GWL_EXSTYLE, ex);
            applyHudOpacity();
            if (mirrorHwnd_ && IsWindow(mirrorHwnd_)) {
                SetForegroundWindow(mirrorHwnd_);
            }
        }
        showToast("CURSOR MODE  (Click phone or 'CFG' ball, " + KeyCodes::toName(profile_.toggleAimVk) + " / F1 for Aim)", 1400);
    }
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayWindow::setEditMode(bool enabled) {
    if (enabled && aimMode_) {
        setAimMode(false);
    }
    if (editMode_ == enabled) return;

    editMode_ = enabled;
    dragging_ = DRAG_NONE;
    rebindTarget_ = REBIND_NONE;
    studioPanel_.clearTextFocus();
    if (editMode_) {
        engine_->releaseAllTouches();
        studioPanel_.setProfilesDir(profilesDir_, profilePath_);
    }

    if (!demoMode_ && hwnd_) {
        LONG ex = GetWindowLongA(hwnd_, GWL_EXSTYLE);
        if (editMode_) {
            ex &= ~WS_EX_TRANSPARENT;
            SetWindowLongA(hwnd_, GWL_EXSTYLE, ex);
            SetLayeredWindowAttributes(hwnd_, 0, 212, LWA_ALPHA);
            SetForegroundWindow(hwnd_);
        } else {
            ex |= WS_EX_TRANSPARENT;
            SetWindowLongA(hwnd_, GWL_EXSTYLE, ex);
            applyHudOpacity();
            if (mirrorHwnd_ && IsWindow(mirrorHwnd_)) {
                SetForegroundWindow(mirrorHwnd_);
            }
        }
    }

    if (editMode_) {
        showToast("KEYMAP STUDIO: Drag panel or keys anywhere | Dbl-Click=Replace | Ctrl+S=Save | F2=Done", 4200);
    } else {
        applyProfileToEngine();
        showToast("Keymap Studio closed", 1200);
    }
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayWindow::toggleFireMode() {
    profile_.fireMode = (profile_.fireMode + 1) % 3;
    applyProfileToEngine();
    const char* msg = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT)
        ? "Fire Mode: DRAG-FIRE (Right Fire Button)"
        : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)
        ? "Fire Mode: CLAW-LEFT (Left Fire + Aim Drag)"
        : "Fire Mode: DUAL-DRAG (Right Fire + Aim Together)";
    showToast(msg, 2200);
}

void OverlayWindow::toggleHud() {
    profile_.showHudInPlay = !profile_.showHudInPlay;
    showToast(profile_.showHudInPlay ? "HUD Badges: VISIBLE" : "HUD Badges: HIDDEN", 1400);
}

void OverlayWindow::triggerQuickReset() {
    engine_->releaseAllTouches();
    showToast("View & Touches Reset!", 900);
}

bool OverlayWindow::isMirrorOrOverlayForeground() const {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == hwnd_) return true;
    if (mirrorHwnd_ && fg == mirrorHwnd_) return true;
    return false;
}

void OverlayWindow::updateClipCursor() {
    if (!aimMode_) return;
    RECT rc = mirrorClientScreenRect_;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    SetCursorPos(cx, cy);
    RECT clipRc = { cx, cy, cx + 1, cy + 1 };
    ClipCursor(&clipRc);
}

bool OverlayWindow::isPointInMirror(POINT screenPt, float& outNormX, float& outNormY) const {
    const RECT& rc = mirrorClientScreenRect_;
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

bool OverlayWindow::isPointInFloatBallScreen(POINT screenPt) const {
    const RECT& rc = mirrorClientScreenRect_;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    int bx = rc.left + static_cast<int>(profile_.floatBtnX * static_cast<float>(w));
    int by = rc.top  + static_cast<int>(profile_.floatBtnY * static_cast<float>(h));
    int dx = screenPt.x - bx;
    int dy = screenPt.y - by;
    int r = HudRenderer::FLOAT_BALL_RADIUS + 4;
    return (dx * dx + dy * dy) <= r * r;
}

void OverlayWindow::syncToMirrorWindow() {
    if (demoMode_) {
        RECT client;
        GetClientRect(hwnd_, &client);
        POINT tl = {client.left, client.top};
        POINT br = {client.right, client.bottom};
        ClientToScreen(hwnd_, &tl);
        ClientToScreen(hwnd_, &br);
        mirrorClientScreenRect_.left = tl.x;
        mirrorClientScreenRect_.top = tl.y;
        mirrorClientScreenRect_.right = br.x;
        mirrorClientScreenRect_.bottom = br.y;
        bool fg = isMirrorOrOverlayForeground();
        if (!fg) {
            if (aimMode_) setAimMode(false);
            if (hadForeground_) engine_->releaseAllTouches();
        }
        hadForeground_ = fg;
        return;
    }

    if (!mirrorHwnd_ || !IsWindow(mirrorHwnd_)) {
        mirrorHwnd_ = FindWindowA(NULL, mirrorTitle_.c_str());
        if (!mirrorHwnd_) {
            if (aimMode_) setAimMode(false);
            if (sawMirrorWindow_ && !bridge_->isMirrorRunning()) {
                PostQuitMessage(0);
            }
            return;
        }
        sawMirrorWindow_ = true;
    }

    if (IsIconic(mirrorHwnd_)) {
        if (aimMode_) setAimMode(false);
        ShowWindow(hwnd_, SW_HIDE);
        return;
    } else if (!IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }

    bool fg = isMirrorOrOverlayForeground();
    if (!fg) {
        if (aimMode_) setAimMode(false);
        if (hadForeground_) {
            engine_->releaseAllTouches();
        }
    }
    hadForeground_ = fg;

    RECT client;
    if (GetClientRect(mirrorHwnd_, &client) && client.right > 0 && client.bottom > 0) {
        POINT tl = {client.left, client.top};
        POINT br = {client.right, client.bottom};
        ClientToScreen(mirrorHwnd_, &tl);
        ClientToScreen(mirrorHwnd_, &br);

        RECT newRc = {tl.x, tl.y, br.x, br.y};
        if (newRc.left != mirrorClientScreenRect_.left ||
            newRc.top != mirrorClientScreenRect_.top ||
            newRc.right != mirrorClientScreenRect_.right ||
            newRc.bottom != mirrorClientScreenRect_.bottom) {
            mirrorClientScreenRect_ = newRc;
            SetWindowPos(hwnd_, HWND_TOPMOST,
                         newRc.left, newRc.top,
                         newRc.right - newRc.left,
                         newRc.bottom - newRc.top,
                         SWP_NOACTIVATE);
            if (aimMode_) updateClipCursor();
        }
    }
}

LRESULT OverlayWindow::handleLowLevelMouse(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (editMode_) {
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
        case WM_XBUTTONDOWN:
            mouseVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            mouseDown = true;
            break;
        case WM_XBUTTONUP:
            mouseVk = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            mouseUp = true;
            break;
        case WM_MOUSEWHEEL: {
            short delta = static_cast<short>(HIWORD(ms->mouseData));
            mouseVk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
            mouseDown = true;
            break;
        }
        default: break;
    }

    if (mouseVk != 0 && (aimMode_ || holdUnlockedAim_ || isMirrorOrOverlayForeground())) {
        if (profile_.holdUnlockVk != 0 && mouseVk == profile_.holdUnlockVk) {
            if (mouseDown && aimMode_ && !holdUnlockedAim_) {
                holdUnlockedAim_ = true;
                PostMessageA(hwnd_, WM_APP + 2, 0, 0);
                return 1;
            }
            if (mouseUp && holdUnlockedAim_) {
                holdUnlockedAim_ = false;
                PostMessageA(hwnd_, WM_APP + 7, 0, 0);
                return 1;
            }
        }
        if (mouseVk == profile_.toggleAimVk) {
            if (mouseDown) {
                PostMessageA(hwnd_, WM_APP + 1, 0, 0);
                return 1;
            }
            if (mouseUp) return 1;
        }
    }

    if (aimMode_) {
        if (!isMirrorOrOverlayForeground()) {
            return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
        }
        if (mouseVk != 0) {
            bool reqToggle = false;
            engine_->onKeyEvent(mouseVk, mouseDown, reqToggle);
            if (reqToggle) {
                PostMessageA(hwnd_, WM_APP + 1, 0, 0);
            }
            return 1;
        }
    } else {
        if (mouseVk != 0 && mouseVk != VK_LBUTTON && mouseDown && isMirrorOrOverlayForeground()) {
            bool reqToggle = false;
            bool consumed = engine_->onKeyEvent(mouseVk, true, reqToggle);
            if (reqToggle) {
                PostMessageA(hwnd_, WM_APP + 1, 0, 0);
            }
            if (consumed) return 1;
        }

        if (wParam == WM_LBUTTONDOWN && isPointInFloatBallScreen(ms->pt)) {
            HWND under = WindowFromPoint(ms->pt);
            if (under == mirrorHwnd_ || under == hwnd_) {
                floatBallHookPressed_ = true;
                floatBallHookMoved_ = false;
                floatBallPressPt_ = ms->pt;
                return 1;
            }
        } else if (wParam == WM_MOUSEMOVE && floatBallHookPressed_) {
            int dx = ms->pt.x - floatBallPressPt_.x;
            int dy = ms->pt.y - floatBallPressPt_.y;
            if (dx * dx + dy * dy >= 36) {
                floatBallHookMoved_ = true;
            }
            if (floatBallHookMoved_) {
                float nx = 0.0f, ny = 0.0f;
                if (isPointInMirror(ms->pt, nx, ny)) {
                    profile_.floatBtnX = clampRange(nx, 0.02f, 0.98f);
                    profile_.floatBtnY = clampRange(ny, 0.05f, 0.95f);
                }
            }
            return 1;
        } else if (wParam == WM_LBUTTONUP && floatBallHookPressed_) {
            bool wasClick = !floatBallHookMoved_;
            floatBallHookPressed_ = false;
            floatBallHookMoved_ = false;
            if (wasClick) {
                PostMessageA(hwnd_, WM_APP + 3, 0, 0);
            }
            return 1;
        }

        if (!demoMode_) {
            float nx = 0.0f, ny = 0.0f;
            bool inMirror = isPointInMirror(ms->pt, nx, ny);
            if (wParam == WM_LBUTTONDOWN && inMirror) {
                HWND under = WindowFromPoint(ms->pt);
                if (under == mirrorHwnd_ || under == hwnd_) {
                    cursorDirectDragging_ = true;
                    engine_->onDirectCursorTouch(TOUCH_DOWN, nx, ny);
                }
            } else if (wParam == WM_MOUSEMOVE && cursorDirectDragging_) {
                if (inMirror) {
                    engine_->onDirectCursorTouch(TOUCH_MOVE, nx, ny);
                }
            } else if (wParam == WM_LBUTTONUP && cursorDirectDragging_) {
                cursorDirectDragging_ = false;
                engine_->onDirectCursorTouch(TOUCH_UP, clamp01(nx), clamp01(ny));
            }
        }
    }

    return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
}

LRESULT OverlayWindow::handleLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    const KBDLLHOOKSTRUCT* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    int vk = static_cast<int>(kb->vkCode);
    bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    if (!down && !up) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    bool isRepeat = false;
    if (vk >= 0 && vk < 256) {
        if (down) {
            isRepeat = hookKeyDown_[vk];
            hookKeyDown_[vk] = true;
        } else if (up) {
            hookKeyDown_[vk] = false;
        }
    }

    if (up && (KeyCodes::matches(vk, profile_.toggleAimVk) || vk == VK_F1)) {
        toggleAimKeyDown_ = false;
        if (!editMode_ && (aimMode_ || isMirrorOrOverlayForeground())) {
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }
    if (up && holdUnlockedAim_ && KeyCodes::matches(vk, profile_.holdUnlockVk)) {
        holdUnlockedAim_ = false;
        if (!editMode_ && isMirrorOrOverlayForeground()) {
            PostMessageA(hwnd_, WM_APP + 7, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (down && (vk == VK_LWIN || vk == VK_RWIN)) {
        if (aimMode_) PostMessageA(hwnd_, WM_APP + 2, 0, 0);
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (!aimMode_ && !isMirrorOrOverlayForeground()) {
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (editMode_) {
        if (down && !isRepeat && vk == VK_F2 && rebindTarget_ == REBIND_NONE &&
            !studioPanel_.isTypingText() && !studioPanel_.isWaitingSpecialRebind()) {
            PostMessageA(hwnd_, WM_APP + 3, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (KeyCodes::matches(vk, profile_.holdUnlockVk)) {
        if (down) {
            if (!isRepeat && aimMode_ && !holdUnlockedAim_) {
                holdUnlockedAim_ = true;
                PostMessageA(hwnd_, WM_APP + 2, 0, 0);
            }
            if (holdUnlockedAim_) return 1;
        }
    }

    if (KeyCodes::matches(vk, profile_.toggleAimVk) || vk == VK_F1) {
        if (down) {
            if (!isRepeat && !toggleAimKeyDown_) {
                toggleAimKeyDown_ = true;
                PostMessageA(hwnd_, WM_APP + 1, 0, 0);
            }
            return 1;
        }
    }

    if (down && isRepeat) {
        if (aimMode_ || engine_->isKeyMapped(vk, false)) {
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (down) {
        if (vk == VK_F2) { PostMessageA(hwnd_, WM_APP + 3, 0, 0); return 1; }
        if (vk == VK_F3) { PostMessageA(hwnd_, WM_APP + 4, 0, 0); return 1; }
        if (vk == VK_F4) { PostMessageA(hwnd_, WM_APP + 5, 0, 0); return 1; }
        if (vk == profile_.resetTouchVk) { PostMessageA(hwnd_, WM_APP + 6, 0, 0); return 1; }
        if (vk == VK_OEM_4)     { PostMessageA(hwnd_, WM_APP + 10, 0, 0); return 1; }
        if (vk == VK_OEM_6)     { PostMessageA(hwnd_, WM_APP + 11, 0, 0); return 1; }
        if (vk == VK_OEM_MINUS) { PostMessageA(hwnd_, WM_APP + 12, 0, 0); return 1; }
        if (vk == VK_OEM_PLUS)  { PostMessageA(hwnd_, WM_APP + 13, 0, 0); return 1; }
    }

    bool reqToggle = false;
    bool consumed = engine_->onKeyEvent(vk, down, reqToggle);
    if (reqToggle) {
        PostMessageA(hwnd_, WM_APP + 1, 0, 0);
    }
    if (consumed) {
        return 1;
    }

    return CallNextHookEx(keyHook_, nCode, wParam, lParam);
}

LRESULT CALLBACK OverlayWindow::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = reinterpret_cast<OverlayWindow*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void OverlayWindow::handleStudioResult(const StudioUIResult& res) {
    if (!res.toastMessage.empty()) {
        showToast(res.toastMessage, 2200);
    }
    if (res.cancelRebind) {
        rebindTarget_ = REBIND_NONE;
    }
    if (res.profileModified) {
        applyProfileToEngine();
    }
    if (res.requestSave) {
        saveCurrentProfile();
    }
    if (res.requestSwitchProfile && !res.switchProfilePath.empty()) {
        Profile loaded;
        if (ProfileStore::load(res.switchProfilePath, loaded)) {
            profilePath_ = res.switchProfilePath;
            profile_ = loaded;
            selectedBtnIndex_ = -1;
            rebindTarget_ = REBIND_NONE;
            ProfileStore::saveLastActive(profilesDir_, profilePath_);
            applyProfileToEngine();
            studioPanel_.setProfilesDir(profilesDir_, profilePath_);
            showToast("Loaded config: " + profile_.name, 2200);
        } else {
            showToast("Failed to load " + res.switchProfilePath, 2500);
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
        selectedBtnIndex_ = static_cast<int>(profile_.buttons.size()) - 1;
        rebindTarget_ = REBIND_BUTTON;
        applyProfileToEngine();
        showToast("Added " + ProfileDefaults::actionTypeToName(n.action) + "! Press ANY Key, Mouse Btn, or Scroll Up/Down.", 3500);
    }
    if (res.requestStartRebindNode && selectedBtnIndex_ >= 0 &&
        selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        rebindTarget_ = REBIND_BUTTON;
        showToast("Press ANY Key, Mouse Btn (LMB/RMB/MMB/X1/X2), Scroll, or Combo for '" + profile_.buttons[selectedBtnIndex_].label + "'...", 3500);
    }
    if (res.requestDeleteSelectedNode && selectedBtnIndex_ >= 0 &&
        selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        profile_.buttons.erase(profile_.buttons.begin() + selectedBtnIndex_);
        selectedBtnIndex_ = -1;
        rebindTarget_ = REBIND_NONE;
        applyProfileToEngine();
        showToast("Key deleted", 1300);
    }
    if (res.requestCloseEdit) {
        setEditMode(false);
    }
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

LRESULT OverlayWindow::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INPUT: {
            if (!aimMode_) break;
            UINT dwSize = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
            if (dwSize > 0 && dwSize <= 256) {
                BYTE rawBuf[256];
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, rawBuf, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
                    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(rawBuf);
                    if (raw->header.dwType == RIM_TYPEMOUSE) {
                        if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                            LONG dx = raw->data.mouse.lLastX;
                            LONG dy = raw->data.mouse.lLastY;
                            if (dx != 0 || dy != 0) {
                                engine_->onRawMouseDelta(static_cast<int>(dx), static_cast<int>(dy));
                            }
                        }
                    }
                }
            }
            return 0;
        }

        case WM_APP + 1: setAimMode(!aimMode_); return 0;
        case WM_APP + 2: setAimMode(false); return 0;
        case WM_APP + 3: setEditMode(!editMode_); return 0;
        case WM_APP + 4: toggleFireMode(); return 0;
        case WM_APP + 5: toggleHud(); return 0;
        case WM_APP + 6: triggerQuickReset(); return 0;
        case WM_APP + 7: setAimMode(true); return 0;

        case WM_APP + 10:
            profile_.sensX = clampRange(profile_.sensX - 0.05f, 0.10f, 10.0f);
            applyProfileToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens X: %.2f  (Y: %.2f)", profile_.sensX, profile_.sensY); showToast(b, 1200); }
            return 0;
        case WM_APP + 11:
            profile_.sensX = clampRange(profile_.sensX + 0.05f, 0.10f, 10.0f);
            applyProfileToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens X: %.2f  (Y: %.2f)", profile_.sensX, profile_.sensY); showToast(b, 1200); }
            return 0;
        case WM_APP + 12:
            profile_.sensY = clampRange(profile_.sensY - 0.05f, 0.10f, 10.0f);
            applyProfileToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens Y: %.2f  (X: %.2f)", profile_.sensY, profile_.sensX); showToast(b, 1200); }
            return 0;
        case WM_APP + 13:
            profile_.sensY = clampRange(profile_.sensY + 0.05f, 0.10f, 10.0f);
            applyProfileToEngine();
            { char b[64]; std::snprintf(b, sizeof(b), "Sens Y: %.2f  (X: %.2f)", profile_.sensY, profile_.sensX); showToast(b, 1200); }
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_SYNC_REPAINT) {
                syncToMirrorWindow();
                DWORD nowTick = GetTickCount();
                bool toastActive = (!toastMsg_.empty() && nowTick < toastExpireTick_);
                bool toastJustExpired = (toastWasVisible_ && !toastActive);
                toastWasVisible_ = toastActive;

                if (editMode_) {
                    if (toastJustExpired) {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                } else {
                    DWORD paintInterval = (aimMode_ && !profile_.showHudInPlay && !demoMode_) ? 500 : 66;
                    if (toastJustExpired || (nowTick - lastPlayPaintTick_) >= paintInterval) {
                        lastPlayPaintTick_ = nowTick;
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                }
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (editMode_) {
                SetCapture(hwnd);
                onEditMouseDown(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), false);
                return 0;
            }
            break;

        case WM_LBUTTONDBLCLK:
            if (editMode_) {
                onEditDoubleClick(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                return 0;
            }
            break;

        case WM_RBUTTONDOWN:
            if (editMode_) {
                onEditMouseDown(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), true);
                return 0;
            }
            break;

        case WM_MBUTTONDOWN:
            if (editMode_ && onEditMouseBind(VK_MBUTTON)) return 0;
            break;

        case WM_XBUTTONDOWN:
            if (editMode_) {
                int xvk = (HIWORD(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                if (onEditMouseBind(xvk)) return 0;
            }
            break;

        case WM_MOUSEMOVE:
            if (editMode_) {
                onEditMouseMove(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                return 0;
            }
            break;

        case WM_LBUTTONUP:
            if (editMode_) {
                ReleaseCapture();
                onEditMouseUp();
                return 0;
            }
            break;

        case WM_MOUSEWHEEL:
            if (editMode_) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int wvk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
                if (onEditMouseBind(wvk)) return 0;
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                ScreenToClient(hwnd, &pt);
                onEditMouseWheel(pt.x, pt.y, delta);
                return 0;
            }
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (editMode_) {
                if (onEditKeyDown(static_cast<int>(wParam))) return 0;
            } else if (msg == WM_SYSKEYDOWN) {
                return 0;
            }
            break;

        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (!editMode_) return 0;
            break;

        case WM_SYSCOMMAND:
            if (!editMode_ && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
            break;

        case WM_CHAR:
            if (editMode_) {
                StudioUIResult res;
                if (studioPanel_.onChar(static_cast<char>(wParam), profile_, selectedBtnIndex_, res)) {
                    handleStudioResult(res);
                    return 0;
                }
            }
            break;

        case WM_SETCURSOR:
            if (aimMode_) {
                SetCursor(NULL);
                return TRUE;
            }
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

        case WM_DESTROY:
            setAimMode(false);
            KillTimer(hwnd, TIMER_SYNC_REPAINT);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

OverlayWindow::DragTargetType OverlayWindow::hitTestNode(int px, int py, int& outBtnIndex) const {
    outBtnIndex = -1;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return DRAG_NONE;

    {
        int bx = static_cast<int>(profile_.floatBtnX * w);
        int by = static_cast<int>(profile_.floatBtnY * h);
        int dx = px - bx, dy = py - by;
        int r = HudRenderer::FLOAT_BALL_RADIUS + 4;
        if (dx * dx + dy * dy <= r * r) return DRAG_FLOAT_BALL;
    }

    for (int i = static_cast<int>(profile_.buttons.size()) - 1; i >= 0; --i) {
        const KeyNode& b = profile_.buttons[i];
        if (b.action != NODE_SWIPE) continue;
        int ex = static_cast<int>(b.x2 * w);
        int ey = static_cast<int>(b.y2 * h);
        int dx = px - ex, dy = py - ey;
        if (dx * dx + dy * dy <= 14 * 14) {
            outBtnIndex = i;
            return DRAG_SWIPE_END;
        }
    }

    for (int i = static_cast<int>(profile_.buttons.size()) - 1; i >= 0; --i) {
        const KeyNode& b = profile_.buttons[i];
        int bx = static_cast<int>(b.x * w);
        int by = static_cast<int>(b.y * h);
        int br = static_cast<int>(b.radius * h);
        if (br < 18) br = 18;
        int dx = px - bx, dy = py - by;
        if (dx * dx + dy * dy <= br * br) {
            outBtnIndex = i;
            return DRAG_BUTTON;
        }
    }

    {
        int fx = static_cast<int>(profile_.fireX * w);
        int fy = static_cast<int>(profile_.fireY * h);
        int dx = px - fx, dy = py - fy;
        if (dx * dx + dy * dy <= 28 * 28) return DRAG_FIRE;
    }
    {
        int lfx = static_cast<int>(profile_.leftFireX * w);
        int lfy = static_cast<int>(profile_.leftFireY * h);
        int dx = px - lfx, dy = py - lfy;
        if (dx * dx + dy * dy <= 24 * 24) return DRAG_LEFT_FIRE;
    }
    {
        int ax = static_cast<int>(profile_.aimX * w);
        int ay = static_cast<int>(profile_.aimY * h);
        int dx = px - ax, dy = py - ay;
        if (dx * dx + dy * dy <= 30 * 30) return DRAG_AIM;
    }
    {
        int jx = static_cast<int>(profile_.joyX * w);
        int jy = static_cast<int>(profile_.joyY * h);
        int jr = static_cast<int>(profile_.joyRadius * h);
        if (jr < 28) jr = 28;
        int dx = px - jx, dy = py - jy;
        if (dx * dx + dy * dy <= jr * jr) return DRAG_JOY;
    }

    return DRAG_NONE;
}

bool OverlayWindow::onEditMouseBind(int mouseVk) {
    StudioUIResult res;
    if (studioPanel_.onMouseBind(mouseVk, profile_, selectedBtnIndex_, res)) {
        handleStudioResult(res);
        return true;
    }

    if (rebindTarget_ == REBIND_NONE) return false;

    if (rebindTarget_ == REBIND_BUTTON &&
        selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        profile_.buttons[selectedBtnIndex_].comboVk = 0;
        profile_.buttons[selectedBtnIndex_].vk = mouseVk;
        if (profile_.buttons[selectedBtnIndex_].label == "KEY") {
            profile_.buttons[selectedBtnIndex_].label = KeyCodes::toName(mouseVk);
        }
        rebindTarget_ = REBIND_NONE;
        applyProfileToEngine();
        showToast("Replaced '" + profile_.buttons[selectedBtnIndex_].label + "' -> " + KeyCodes::toName(mouseVk), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    if (rebindTarget_ == REBIND_FIRE_KEY) {
        profile_.fireVk = mouseVk;
        rebindTarget_ = REBIND_NONE;
        applyProfileToEngine();
        showToast("Fire key replaced -> " + KeyCodes::toName(mouseVk), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    if (rebindTarget_ == REBIND_TOGGLE_AIM) {
        if (mouseVk == VK_LBUTTON) {
            showToast("Use Keyboard, MMB, RMB, X1, X2, or Wheel for Aim Lock (LMB is reserved)", 2400);
            return true;
        }
        profile_.toggleAimVk = mouseVk;
        rebindTarget_ = REBIND_NONE;
        applyProfileToEngine();
        showToast("Aim Lock/Unlock key replaced -> " + KeyCodes::toName(mouseVk), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    return false;
}

void OverlayWindow::onEditDoubleClick(int x, int y) {
    if (studioPanel_.containsPoint(x, y)) {
        SetCapture(hwnd_);
        onEditMouseDown(x, y, false);
        return;
    }

    int btnIdx = -1;
    DragTargetType t = hitTestNode(x, y, btnIdx);
    if (t == DRAG_BUTTON && btnIdx >= 0 && btnIdx < static_cast<int>(profile_.buttons.size())) {
        selectedBtnIndex_ = btnIdx;
        dragging_ = DRAG_NONE;
        rebindTarget_ = REBIND_BUTTON;
        studioPanel_.clearTextFocus();
        showToast("Replace '" + profile_.buttons[btnIdx].label + "': Press ANY key or Mouse button (LMB/RMB/MMB/X1/X2/Scroll)", 4500);
        InvalidateRect(hwnd_, NULL, FALSE);
    } else if (t == DRAG_FIRE || t == DRAG_LEFT_FIRE) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = REBIND_FIRE_KEY;
        studioPanel_.clearTextFocus();
        showToast("Replace FIRE key: Press ANY key or Mouse button (LMB/RMB/MMB/X1/X2)", 4500);
        InvalidateRect(hwnd_, NULL, FALSE);
    } else if (t == DRAG_AIM) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = REBIND_TOGGLE_AIM;
        studioPanel_.clearTextFocus();
        showToast("Replace AIM LOCK/UNLOCK key: Press ANY key (e.g. LShift, ~) or Mouse button (e.g. MMB)", 4500);
        InvalidateRect(hwnd_, NULL, FALSE);
    }
}

void OverlayWindow::onEditMouseDown(int x, int y, bool rightButton) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    if (!rightButton && studioPanel_.containsPoint(x, y)) {
        StudioUIResult res;
        if (studioPanel_.onMouseDown(x, y, w, h, profile_, selectedBtnIndex_, res)) {
            handleStudioResult(res);
            return;
        }
    }

    if (rebindTarget_ != REBIND_NONE || studioPanel_.isWaitingSpecialRebind()) {
        if (onEditMouseBind(rightButton ? VK_RBUTTON : VK_LBUTTON)) {
            return;
        }
    }

    if (rightButton && !studioPanel_.containsPoint(x, y)) {
        KeyNode n;
        n.label = "KEY";
        n.vk = 'K';
        n.action = NODE_PRESS;
        n.x = clamp01(static_cast<float>(x) / static_cast<float>(w));
        n.y = clamp01(static_cast<float>(y) / static_cast<float>(h));
        n.x2 = n.x;
        n.y2 = n.y;
        n.radius = 0.030f;
        profile_.buttons.push_back(n);
        selectedBtnIndex_ = static_cast<int>(profile_.buttons.size()) - 1;
        rebindTarget_ = REBIND_BUTTON;
        applyProfileToEngine();
        showToast("New Key placed! Press ANY key or Mouse button (MMB/RMB/X1/X2/Scroll) to bind...", 3500);
        InvalidateRect(hwnd_, NULL, FALSE);
        return;
    }

    int btnIdx = -1;
    dragging_ = hitTestNode(x, y, btnIdx);
    dragBtnIndex_ = btnIdx;
    if (dragging_ == DRAG_BUTTON || dragging_ == DRAG_SWIPE_END) {
        selectedBtnIndex_ = btnIdx;
    } else if (dragging_ == DRAG_NONE) {
        selectedBtnIndex_ = -1;
    }
    InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayWindow::onEditMouseMove(int x, int y) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    StudioUIResult res;
    if (studioPanel_.onMouseMove(x, y, w, h, profile_, selectedBtnIndex_, res)) {
        handleStudioResult(res);
        return;
    }

    if (dragging_ == DRAG_NONE) return;

    float nx = clamp01(static_cast<float>(x) / static_cast<float>(w));
    float ny = clamp01(static_cast<float>(y) / static_cast<float>(h));

    switch (dragging_) {
        case DRAG_FLOAT_BALL:
            profile_.floatBtnX = clampRange(nx, 0.02f, 0.98f);
            profile_.floatBtnY = clampRange(ny, 0.05f, 0.95f);
            break;
        case DRAG_JOY:       profile_.joyX = nx; profile_.joyY = ny; break;
        case DRAG_AIM:       profile_.aimX = nx; profile_.aimY = ny; break;
        case DRAG_FIRE:      profile_.fireX = nx; profile_.fireY = ny; break;
        case DRAG_LEFT_FIRE: profile_.leftFireX = nx; profile_.leftFireY = ny; break;
        case DRAG_BUTTON:
            if (dragBtnIndex_ >= 0 && dragBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
                KeyNode& b = profile_.buttons[dragBtnIndex_];
                float dx = nx - b.x;
                float dy = ny - b.y;
                b.x = nx;
                b.y = ny;
                if (b.action == NODE_SWIPE) {
                    b.x2 = clamp01(b.x2 + dx);
                    b.y2 = clamp01(b.y2 + dy);
                }
            }
            break;
        case DRAG_SWIPE_END:
            if (dragBtnIndex_ >= 0 && dragBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
                profile_.buttons[dragBtnIndex_].x2 = nx;
                profile_.buttons[dragBtnIndex_].y2 = ny;
            }
            break;
        default: break;
    }
    InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayWindow::onEditMouseUp() {
    studioPanel_.onMouseUp();
    if (dragging_ != DRAG_NONE) {
        dragging_ = DRAG_NONE;
        applyProfileToEngine();
    }
}

void OverlayWindow::onEditMouseWheel(int x, int y, int delta) {
    if (studioPanel_.containsPoint(x, y)) return;

    int btnIdx = -1;
    DragTargetType t = hitTestNode(x, y, btnIdx);
    float step = (delta > 0) ? 0.010f : -0.010f;
    bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if (t == DRAG_JOY) {
        if (shiftHeld) {
            profile_.joySprintRadius = clampRange(profile_.joySprintRadius + step, 0.04f, 0.35f);
            showToast("Joystick Sprint Radius updated", 900);
        } else {
            profile_.joyRadius = clampRange(profile_.joyRadius + step, 0.03f, 0.25f);
            showToast("Joystick Radius updated", 900);
        }
    } else if (t == DRAG_AIM) {
        if (shiftHeld) {
            profile_.aimHalfH = clampRange(profile_.aimHalfH + step, 0.06f, 0.80f);
        } else {
            profile_.aimHalfW = clampRange(profile_.aimHalfW + step, 0.06f, 0.45f);
            profile_.aimHalfH = clampRange(profile_.aimHalfH + step, 0.06f, 0.80f);
        }
        showToast("Aim Box resized", 900);
    } else if (t == DRAG_FIRE) {
        profile_.fireBoxHalfH = clampRange(profile_.fireBoxHalfH + step, 0.10f, 0.90f);
        showToast("Fire Drag Box resized", 900);
    } else {
        return;
    }
    applyProfileToEngine();
    InvalidateRect(hwnd_, NULL, FALSE);
}

bool OverlayWindow::onEditKeyDown(int vk) {
    StudioUIResult res;
    if (studioPanel_.onKeyDown(vk, profile_, selectedBtnIndex_, res)) {
        handleStudioResult(res);
        return true;
    }

    if (rebindTarget_ != REBIND_NONE) {
        if (vk == VK_ESCAPE) {
            rebindTarget_ = REBIND_NONE;
            showToast("Rebind cancelled", 1000);
            InvalidateRect(hwnd_, NULL, FALSE);
            return true;
        }
        vk = KeyCodes::normalizeModifier(vk);

        if (rebindTarget_ == REBIND_FIRE_KEY) {
            profile_.fireVk = vk;
            rebindTarget_ = REBIND_NONE;
            applyProfileToEngine();
            showToast("Fire key replaced -> " + KeyCodes::toName(vk), 1800);
            InvalidateRect(hwnd_, NULL, FALSE);
            return true;
        }
        if (rebindTarget_ == REBIND_TOGGLE_AIM) {
            profile_.toggleAimVk = vk;
            rebindTarget_ = REBIND_NONE;
            applyProfileToEngine();
            showToast("Aim Lock/Unlock key replaced -> " + KeyCodes::toName(vk), 1800);
            InvalidateRect(hwnd_, NULL, FALSE);
            return true;
        }
        if (rebindTarget_ == REBIND_BUTTON &&
            selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
            int comboPrefix = 0;
            if (vk != VK_LMENU && vk != VK_RMENU && (GetAsyncKeyState(VK_LMENU) & 0x8000)) comboPrefix = VK_LMENU;
            else if (vk != VK_LCONTROL && vk != VK_RCONTROL && (GetAsyncKeyState(VK_LCONTROL) & 0x8000)) comboPrefix = VK_LCONTROL;
            else if (vk != VK_LSHIFT && vk != VK_RSHIFT && (GetAsyncKeyState(VK_LSHIFT) & 0x8000)) comboPrefix = VK_LSHIFT;

            profile_.buttons[selectedBtnIndex_].comboVk = comboPrefix;
            profile_.buttons[selectedBtnIndex_].vk = vk;
            if (profile_.buttons[selectedBtnIndex_].label == "KEY") {
                profile_.buttons[selectedBtnIndex_].label = KeyCodes::comboToName(comboPrefix, vk);
            }
            rebindTarget_ = REBIND_NONE;
            applyProfileToEngine();
            showToast("Replaced '" + profile_.buttons[selectedBtnIndex_].label + "' -> " + KeyCodes::comboToName(comboPrefix, vk), 1800);
            InvalidateRect(hwnd_, NULL, FALSE);
        }
        return true;
    }

    bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrlHeld && vk == 'S') {
        saveCurrentProfile();
        return true;
    }
    if (vk == VK_F2 || vk == VK_ESCAPE) {
        setEditMode(false);
        return true;
    }
    if (vk == 'T' && selectedBtnIndex_ >= 0 &&
        selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        KeyNode& b = profile_.buttons[selectedBtnIndex_];
        b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 1) % 10);
        if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) {
            b.x2 = clamp01(b.x + 0.12f);
            b.y2 = b.y;
            b.durationMs = 120;
        }
        applyProfileToEngine();
        showToast("Node '" + b.label + "' -> " + ProfileDefaults::actionTypeToName(b.action), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    if (vk == VK_RETURN && selectedBtnIndex_ >= 0) {
        rebindTarget_ = REBIND_BUTTON;
        showToast("Replace '" + profile_.buttons[selectedBtnIndex_].label + "': Press ANY key or Mouse button (LMB/RMB/MMB/X1/X2/Scroll)...", 4000);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    if (vk == VK_DELETE && selectedBtnIndex_ >= 0 &&
        selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        profile_.buttons.erase(profile_.buttons.begin() + selectedBtnIndex_);
        selectedBtnIndex_ = -1;
        applyProfileToEngine();
        showToast("Key deleted", 1200);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    return false;
}

void OverlayWindow::onPaint(HDC hdc) {
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
    HDC memDC = cacheDC_;

    COLORREF bgColor = COLOR_KEY_TRANSPARENT;
    if (demoMode_) bgColor = RGB(14, 18, 26);
    else if (editMode_) bgColor = RGB(12, 16, 24);
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    if (demoMode_) {
        HudRenderer::paintDemoBackdrop(memDC, w, h, fonts_);
    }

    EngineTelemetry tel = engine_->getTelemetry();
    if (editMode_) {
        HudRenderer::paintKeymapNodes(memDC, w, h, profile_, tel, true, demoMode_,
                                      selectedBtnIndex_, (rebindTarget_ == REBIND_BUTTON), fonts_);
        HudRenderer::paintFloatingBall(memDC, w, h, profile_, true, fonts_);
        studioPanel_.paint(memDC, w, h, profile_, profilePath_,
                           selectedBtnIndex_, (rebindTarget_ == REBIND_BUTTON),
                           fonts_.smallFont, fonts_.boldFont, fonts_.titleFont);
        if (rebindTarget_ != REBIND_NONE || studioPanel_.isWaitingSpecialRebind()) {
            std::string targetName = "KEY";
            bool specialAim = false;
            if (studioPanel_.isRebindingToggleAim() || rebindTarget_ == REBIND_TOGGLE_AIM) {
                targetName = "AIM LOCK / UNLOCK";
                specialAim = true;
            } else if (studioPanel_.isRebindingHoldUnlock()) {
                targetName = "HOLD-TO-UNLOCK CURSOR";
                specialAim = true;
            } else if (rebindTarget_ == REBIND_FIRE_KEY) {
                targetName = "FIRE";
            } else if (rebindTarget_ == REBIND_BUTTON &&
                       selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
                targetName = profile_.buttons[selectedBtnIndex_].label;
            }
            HudRenderer::paintRebindPrompt(memDC, w, h, targetName, specialAim, fonts_);
        }
    } else {
        if (profile_.showHudInPlay || demoMode_) {
            HudRenderer::paintKeymapNodes(memDC, w, h, profile_, tel, false, demoMode_,
                                          -1, false, fonts_);
        }
        HudRenderer::paintFloatingBall(memDC, w, h, profile_, false, fonts_);
        if (aimMode_ && !demoMode_) {
            int cx = w / 2, cy = h / 2;
            RECT dot = { cx - 1, cy - 1, cx + 2, cy + 2 };
            HBRUSH dotBrush = CreateSolidBrush(RGB(0, 255, 160));
            FillRect(memDC, &dot, dotBrush);
            DeleteObject(dotBrush);
        }
        HudRenderer::paintPlayStatusBar(memDC, w, profile_, tel, fonts_);
    }

    if (!toastMsg_.empty() && GetTickCount() < toastExpireTick_) {
        HudRenderer::paintToast(memDC, w, h, toastMsg_, fonts_);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
}
