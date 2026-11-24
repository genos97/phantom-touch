#include "overlay.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

static OverlayApp* g_overlayInstance = NULL;

static const COLORREF COLOR_KEY_TRANSPARENT = RGB(255, 0, 255);
static const UINT_PTR TIMER_SYNC_REPAINT = 1;
static const int FLOAT_BALL_RADIUS = 19;

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

bool vkMatchesKey(int hookVk, int targetVk) {
    if (targetVk == 0) return false;
    if (hookVk == targetVk) return true;
    // Also match generic Shift/Ctrl/Alt in case a config stored 0x10/0x11/0x12
    if (targetVk == VK_SHIFT   && (hookVk == VK_LSHIFT   || hookVk == VK_RSHIFT))   return true;
    if (targetVk == VK_CONTROL && (hookVk == VK_LCONTROL || hookVk == VK_RCONTROL)) return true;
    if (targetVk == VK_MENU    && (hookVk == VK_LMENU    || hookVk == VK_RMENU))    return true;
    return false;
}

int normalizeKeyVk(int vk) {
    if (vk == VK_SHIFT)   return ((GetKeyState(VK_RSHIFT)   & 0x8000) || (GetAsyncKeyState(VK_RSHIFT)   & 0x8000)) ? VK_RSHIFT   : VK_LSHIFT;
    if (vk == VK_CONTROL) return ((GetKeyState(VK_RCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000)) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU)    return ((GetKeyState(VK_RMENU)    & 0x8000) || (GetAsyncKeyState(VK_RMENU)    & 0x8000)) ? VK_RMENU    : VK_LMENU;
    return vk;
}

void drawCenteredText(HDC hdc, const std::string& text, int cx, int cy, COLORREF color) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text.c_str(), static_cast<int>(text.size()), &sz);
    TextOutA(hdc, cx - sz.cx / 2, cy - sz.cy / 2, text.c_str(), static_cast<int>(text.size()));
}

void drawCircle(HDC hdc, int cx, int cy, int r, COLORREF fill, COLORREF border, int borderW = 2, bool hollow = false) {
    HPEN pen = CreatePen(PS_SOLID, borderW, border);
    HBRUSH brush = hollow ? static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)) : CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    Ellipse(hdc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    if (!hollow) DeleteObject(brush);
}

void drawRoundedBox(HDC hdc, const RECT& rc, int rad, COLORREF fill, COLORREF border, int borderW = 1, bool hollow = false) {
    HPEN pen = CreatePen(PS_SOLID, borderW, border);
    HBRUSH brush = hollow ? static_cast<HBRUSH>(GetStockObject(NULL_BRUSH)) : CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, rad, rad);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    if (!hollow) DeleteObject(brush);
}

} // namespace

OverlayApp::OverlayApp(ScrcpyBridge* bridge, AimEngine* engine,
                       const std::string& profilesDir, const std::string& profilePath,
                       const std::string& mirrorTitle, bool demoMode)
    : bridge_(bridge), engine_(engine), profilesDir_(profilesDir),
      profilePath_(profilePath), mirrorTitle_(mirrorTitle), demoMode_(demoMode) {
    ZeroMemory(&mirrorClientScreenRect_, sizeof(mirrorClientScreenRect_));
    ZeroMemory(&floatBallPressPt_, sizeof(floatBallPressPt_));
    std::memset(hookKeyDown_, 0, sizeof(hookKeyDown_));
    profile_ = engine_->getProfileCopy();
    configUI_.setProfilesDir(profilesDir_, profilePath_);
}

OverlayApp::~OverlayApp() {
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
    if (fontSmall_) DeleteObject(fontSmall_);
    if (fontBold_) DeleteObject(fontBold_);
    if (fontTitle_) DeleteObject(fontTitle_);
    if (g_overlayInstance == this) g_overlayInstance = NULL;
}

bool OverlayApp::create(HINSTANCE hInst) {
    hInst_ = hInst;
    g_overlayInstance = this;

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = OverlayApp::wndProc;
    wc.hInstance = hInst_;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ScrcpyKeymapperOverlayClass";

    if (!RegisterClassExA(&wc)) {
        return false;
    }

    fontSmall_ = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    fontBold_  = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    fontTitle_ = CreateFontA(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    if (demoMode_) {
        DWORD style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        RECT rc = {0, 0, 1280, 720};
        AdjustWindowRect(&rc, style, FALSE);
        hwnd_ = CreateWindowExA(0, wc.lpszClassName,
                                "Scrcpy Keymapper Studio [DEMO / OFFLINE EDITOR]",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                NULL, NULL, hInst_, this);
    } else {
        DWORD exStyle = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW;
        hwnd_ = CreateWindowExA(exStyle, wc.lpszClassName,
                                "Scrcpy Keymapper Overlay",
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
        showToast("Connected! Click floating 'CFG' ball (or F2) for Studio | ~ or F1 for Aim Mode", 4500);
    }
    return true;
}

void OverlayApp::applyHudOpacity() {
    if (demoMode_ || !hwnd_ || editMode_) return;
    BYTE alpha = static_cast<BYTE>(clampRange(profile_.hudOpacity, 0.15f, 1.00f) * 255.0f);
    SetLayeredWindowAttributes(hwnd_, COLOR_KEY_TRANSPARENT, alpha, LWA_COLORKEY | LWA_ALPHA);
}

void OverlayApp::registerRawInput() {
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x01; // HID_USAGE_PAGE_GENERIC
    rid.usUsage     = 0x02; // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void OverlayApp::installHooks() {
    mouseHook_ = SetWindowsHookExA(WH_MOUSE_LL, globalMouseHookProc, hInst_, 0);
    keyHook_   = SetWindowsHookExA(WH_KEYBOARD_LL, globalKeyHookProc, hInst_, 0);
}

void OverlayApp::removeHooks() {
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = NULL;
    }
    if (keyHook_) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = NULL;
    }
}

int OverlayApp::runMessageLoop() {
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void OverlayApp::showToast(const std::string& msg, DWORD durationMs) {
    toastMsg_ = msg;
    toastExpireTick_ = GetTickCount() + durationMs;
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayApp::applyProfileToEngine() {
    engine_->setProfile(profile_);
    applyHudOpacity();
}

void OverlayApp::saveCurrentProfile() {
    if (saveProfile(profilePath_, profile_)) {
        saveLastActiveProfilePath(profilesDir_, profilePath_);
        configUI_.refreshProfileList();
        showToast("Saved config -> " + profilePath_, 2200);
    } else {
        showToast("ERROR: Could not write " + profilePath_, 3000);
    }
}

void OverlayApp::setAimMode(bool enabled) {
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
        showToast("AIM MODE ON  (Press " + vkToName(profile_.toggleAimVk) + " or F1 to release mouse)", 1400);
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
        showToast("CURSOR MODE  (Click phone or 'CFG' ball, " + vkToName(profile_.toggleAimVk) + " / F1 for Aim)", 1400);
    }
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayApp::setEditMode(bool enabled) {
    if (enabled && aimMode_) {
        setAimMode(false);
    }
    if (editMode_ == enabled) return;

    editMode_ = enabled;
    dragging_ = DRAG_NONE;
    rebindTarget_ = REBIND_NONE;
    configUI_.clearTextFocus();
    if (editMode_) {
        engine_->releaseAllTouches();
        configUI_.setProfilesDir(profilesDir_, profilePath_);
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
        showToast("KEYMAP STUDIO: Drag panel or keys anywhere | Wheel=Resize | Ctrl+S=Save | F2=Done", 4200);
    } else {
        applyProfileToEngine();
        showToast("Keymap Studio closed", 1200);
    }
    if (hwnd_) InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayApp::toggleFireMode() {
    profile_.fireMode = (profile_.fireMode + 1) % 3;
    applyProfileToEngine();
    const char* msg = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT)
        ? "Fire Mode: DRAG-FIRE (Right Fire Button)"
        : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)
        ? "Fire Mode: CLAW-LEFT (Left Fire + Aim Drag)"
        : "Fire Mode: DUAL-DRAG (Right Fire + Aim Together)";
    showToast(msg, 2200);
}

void OverlayApp::toggleHud() {
    profile_.showHudInPlay = !profile_.showHudInPlay;
    showToast(profile_.showHudInPlay ? "HUD Badges: VISIBLE" : "HUD Badges: HIDDEN", 1400);
}

void OverlayApp::triggerQuickReset() {
    engine_->releaseAllTouches();
    showToast("View & Touches Reset!", 900);
}

bool OverlayApp::isMirrorOrOverlayForeground() const {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (fg == hwnd_) return true;
    if (mirrorHwnd_ && fg == mirrorHwnd_) return true;
    return false;
}

void OverlayApp::updateClipCursor() {
    if (!aimMode_) return;
    RECT rc = mirrorClientScreenRect_;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    int cx = (rc.left + rc.right) / 2;
    int cy = (rc.top + rc.bottom) / 2;
    SetCursorPos(cx, cy);
    RECT clipRc = { cx, cy, cx + 1, cy + 1 };
    ClipCursor(&clipRc);
}

bool OverlayApp::isPointInMirror(POINT screenPt, float& outNormX, float& outNormY) const {
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

bool OverlayApp::isPointInFloatBallScreen(POINT screenPt) const {
    const RECT& rc = mirrorClientScreenRect_;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    int bx = rc.left + static_cast<int>(profile_.floatBtnX * static_cast<float>(w));
    int by = rc.top  + static_cast<int>(profile_.floatBtnY * static_cast<float>(h));
    int dx = screenPt.x - bx;
    int dy = screenPt.y - by;
    return (dx * dx + dy * dy) <= (FLOAT_BALL_RADIUS + 4) * (FLOAT_BALL_RADIUS + 4);
}

void OverlayApp::syncToMirrorWindow() {
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
        if (aimMode_ && !isMirrorOrOverlayForeground()) {
            setAimMode(false);
        }
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

LRESULT OverlayApp::handleLowLevelMouse(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !lParam) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    const MSLLHOOKSTRUCT* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);

    if (editMode_) {
        return CallNextHookEx(mouseHook_, nCode, wParam, lParam);
    }

    // Helper: extract mouse button VK for both Aim Mode and Cursor Mode (supports MMB/X1/X2 Aim Lock!)
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

    // Handle mouse-bound Aim Lock / Hold-Unlock in BOTH Aim Mode and Cursor Mode!
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

    // In Aim Mode: route mouse buttons & wheel to AimEngine and swallow OS clicks
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
        // Cursor Mode: allow NODE_CURSOR_TOGGLE keys bound to non-LMB mouse controls (e.g. MMB/X1/X2/RMB) to close Bag & re-lock Aim!
        if (mouseVk != 0 && mouseVk != VK_LBUTTON && mouseDown && isMirrorOrOverlayForeground()) {
            bool reqToggle = false;
            bool consumed = engine_->onKeyEvent(mouseVk, true, reqToggle);
            if (reqToggle) {
                PostMessageA(hwnd_, WM_APP + 1, 0, 0);
            }
            if (consumed) return 1;
        }

        // Cursor Mode (Aim OFF): Check GG-style Floating Config Ball FIRST (click to open Studio, drag to move!)
        if (wParam == WM_LBUTTONDOWN && isPointInFloatBallScreen(ms->pt)) {
            HWND under = WindowFromPoint(ms->pt);
            if (under == mirrorHwnd_ || under == hwnd_) {
                floatBallHookPressed_ = true;
                floatBallHookMoved_ = false;
                floatBallPressPt_ = ms->pt;
                return 1; // Swallow so phone isn't tapped under the ball!
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
                PostMessageA(hwnd_, WM_APP + 3, 0, 0); // Open Keymap Studio!
            }
            return 1;
        }

        // Otherwise forward direct clicks on the mirror window to the phone!
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

LRESULT OverlayApp::handleLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam) {
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

    // Track real physical down/up edges independent of AimEngine state resets (kills Windows key auto-repeat!)
    bool isRepeat = false;
    if (vk >= 0 && vk < 256) {
        if (down) {
            isRepeat = hookKeyDown_[vk];
            hookKeyDown_[vk] = true;
        } else if (up) {
            hookKeyDown_[vk] = false;
        }
    }

    // Always release edge-trigger latches on key-up even if focus transiently shifted!
    if (up && (vkMatchesKey(vk, profile_.toggleAimVk) || vk == VK_F1)) {
        toggleAimKeyDown_ = false;
        if (!editMode_ && (aimMode_ || isMirrorOrOverlayForeground())) {
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }
    if (up && holdUnlockedAim_ && vkMatchesKey(vk, profile_.holdUnlockVk)) {
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
            !configUI_.isTypingText() && !configUI_.isWaitingSpecialRebind()) {
            PostMessageA(hwnd_, WM_APP + 3, 0, 0);
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    // Hold-to-Unlock cursor key (matches LShift/RShift/LCtrl/LAlt cleanly + ignores auto-repeat!)
    if (vkMatchesKey(vk, profile_.holdUnlockVk)) {
        if (down) {
            if (!isRepeat && aimMode_ && !holdUnlockedAim_) {
                holdUnlockedAim_ = true;
                PostMessageA(hwnd_, WM_APP + 2, 0, 0);
            }
            if (holdUnlockedAim_) return 1;
        }
    }

    // Aim Lock Toggle key (edge-triggered so holding LShift / ~ never auto-repeat double-toggles!)
    if (vkMatchesKey(vk, profile_.toggleAimVk) || vk == VK_F1) {
        if (down) {
            if (!isRepeat && !toggleAimKeyDown_) {
                toggleAimKeyDown_ = true;
                PostMessageA(hwnd_, WM_APP + 1, 0, 0);
            }
            return 1;
        }
    }

    // Suppress Windows keyboard auto-repeat while playing (in BOTH Aim Mode & Cursor Mode!)
    // so holding W/A/S/D, LShift, or any mapped key never double-toggles or double-taps!
    if (down && isRepeat) {
        if (aimMode_ || engine_->isKeyMapped(vk, false)) {
            return 1;
        }
        return CallNextHookEx(keyHook_, nCode, wParam, lParam);
    }

    if (down) {
        if (vk == VK_F2) {
            PostMessageA(hwnd_, WM_APP + 3, 0, 0);
            return 1;
        }
        if (vk == VK_F3) {
            PostMessageA(hwnd_, WM_APP + 4, 0, 0);
            return 1;
        }
        if (vk == VK_F4) {
            PostMessageA(hwnd_, WM_APP + 5, 0, 0);
            return 1;
        }
        if (vk == profile_.resetTouchVk) {
            PostMessageA(hwnd_, WM_APP + 6, 0, 0);
            return 1;
        }
        if (vk == VK_OEM_4)     { PostMessageA(hwnd_, WM_APP + 10, 0, 0); return 1; } // '['
        if (vk == VK_OEM_6)     { PostMessageA(hwnd_, WM_APP + 11, 0, 0); return 1; } // ']'
        if (vk == VK_OEM_MINUS) { PostMessageA(hwnd_, WM_APP + 12, 0, 0); return 1; } // '-'
        if (vk == VK_OEM_PLUS)  { PostMessageA(hwnd_, WM_APP + 13, 0, 0); return 1; } // '='
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

LRESULT CALLBACK OverlayApp::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    OverlayApp* self = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = reinterpret_cast<OverlayApp*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OverlayApp*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->handleMessage(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void OverlayApp::handleConfigUIResult(const ConfigUIResult& res) {
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
        if (loadProfile(res.switchProfilePath, loaded)) {
            profilePath_ = res.switchProfilePath;
            profile_ = loaded;
            selectedBtnIndex_ = -1;
            rebindTarget_ = REBIND_NONE;
            saveLastActiveProfilePath(profilesDir_, profilePath_);
            applyProfileToEngine();
            configUI_.setProfilesDir(profilesDir_, profilePath_);
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
        showToast("Added " + actionTypeToName(n.action) + "! Press ANY Key or Mouse Btn (MMB/RMB/X1/X2) to bind.", 3500);
    }
    if (res.requestStartRebindNode && selectedBtnIndex_ >= 0 &&
        selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        rebindTarget_ = REBIND_BUTTON;
        showToast("Press ANY Key, Mouse Btn (LMB/RMB/MMB/X1/X2), Wheel, or Combo for '" + profile_.buttons[selectedBtnIndex_].label + "'...", 3500);
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

LRESULT OverlayApp::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
                    // Edit Mode repaints immediately on mouse/key events; only timer-repaint on toast expiry
                    if (toastJustExpired) {
                        InvalidateRect(hwnd, NULL, FALSE);
                    }
                } else {
                    // Throttle layered overlay repaints during live gameplay so DWM & WM_INPUT never stutter!
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
            if (editMode_) {
                if (onEditMouseBind(VK_MBUTTON)) return 0;
            }
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
                return 0; // Never let Alt/F10 activate hidden Win32 menu bar and freeze mouse/keyboard!
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
                ConfigUIResult res;
                if (configUI_.onChar(static_cast<char>(wParam), profile_, selectedBtnIndex_, res)) {
                    handleConfigUIResult(res);
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

OverlayApp::DragTargetType OverlayApp::hitTestNode(int px, int py, int& outBtnIndex) const {
    outBtnIndex = -1;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return DRAG_NONE;

    // Floating CFG ball
    {
        int bx = static_cast<int>(profile_.floatBtnX * w);
        int by = static_cast<int>(profile_.floatBtnY * h);
        int dx = px - bx, dy = py - by;
        if (dx * dx + dy * dy <= (FLOAT_BALL_RADIUS + 4) * (FLOAT_BALL_RADIUS + 4)) {
            return DRAG_FLOAT_BALL;
        }
    }

    // Swipe end handles first so user can grab endpoint cleanly
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

    // Action buttons
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

    // Right Fire
    {
        int fx = static_cast<int>(profile_.fireX * w);
        int fy = static_cast<int>(profile_.fireY * h);
        int dx = px - fx, dy = py - fy;
        if (dx * dx + dy * dy <= 28 * 28) return DRAG_FIRE;
    }
    // Left Fire
    {
        int lfx = static_cast<int>(profile_.leftFireX * w);
        int lfy = static_cast<int>(profile_.leftFireY * h);
        int dx = px - lfx, dy = py - lfy;
        if (dx * dx + dy * dy <= 24 * 24) return DRAG_LEFT_FIRE;
    }
    // Aim anchor
    {
        int ax = static_cast<int>(profile_.aimX * w);
        int ay = static_cast<int>(profile_.aimY * h);
        int dx = px - ax, dy = py - ay;
        if (dx * dx + dy * dy <= 30 * 30) return DRAG_AIM;
    }
    // WASD Joystick
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

bool OverlayApp::onEditMouseBind(int mouseVk) {
    // 1. Check if Advanced tab is waiting for Aim Lock or Hold-Unlock rebind
    ConfigUIResult res;
    if (configUI_.onMouseBind(mouseVk, profile_, selectedBtnIndex_, res)) {
        handleConfigUIResult(res);
        return true;
    }

    if (rebindTarget_ == REBIND_NONE) return false;

    if (rebindTarget_ == REBIND_BUTTON &&
        selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        profile_.buttons[selectedBtnIndex_].comboVk = 0;
        profile_.buttons[selectedBtnIndex_].vk = mouseVk;
        if (profile_.buttons[selectedBtnIndex_].label == "KEY") {
            profile_.buttons[selectedBtnIndex_].label = vkToName(mouseVk);
        }
        rebindTarget_ = REBIND_NONE;
        applyProfileToEngine();
        showToast("Replaced '" + profile_.buttons[selectedBtnIndex_].label + "' -> " + vkToName(mouseVk), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    if (rebindTarget_ == REBIND_FIRE_KEY) {
        profile_.fireVk = mouseVk;
        rebindTarget_ = REBIND_NONE;
        applyProfileToEngine();
        showToast("Fire key replaced -> " + vkToName(mouseVk), 1800);
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
        showToast("Aim Lock/Unlock key replaced -> " + vkToName(mouseVk), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    return false;
}

void OverlayApp::onEditDoubleClick(int x, int y) {
    if (configUI_.containsPoint(x, y)) {
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
        configUI_.clearTextFocus();
        showToast("Replace '" + profile_.buttons[btnIdx].label + "': Press ANY key or Mouse button (LMB/RMB/MMB/X1/X2/Wheel)", 4500);
        InvalidateRect(hwnd_, NULL, FALSE);
    } else if (t == DRAG_FIRE || t == DRAG_LEFT_FIRE) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = REBIND_FIRE_KEY;
        configUI_.clearTextFocus();
        showToast("Replace FIRE key: Press ANY key or Mouse button (LMB/RMB/MMB/X1/X2)", 4500);
        InvalidateRect(hwnd_, NULL, FALSE);
    } else if (t == DRAG_AIM) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = REBIND_TOGGLE_AIM;
        configUI_.clearTextFocus();
        showToast("Replace AIM LOCK/UNLOCK key: Press ANY key (e.g. LShift, ~) or Mouse button (e.g. MMB)", 4500);
        InvalidateRect(hwnd_, NULL, FALSE);
    }
}

void OverlayApp::onEditMouseDown(int x, int y, bool rightButton) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // 1. Left-click inside Studio Panel / Key Inspector ALWAYS interacts with UI buttons
    //    (so clicking MMB / LMB / RMB / X1 / X2 / W+ / W- chips or tabs works even while rebind prompt is open!)
    if (!rightButton && configUI_.containsPoint(x, y)) {
        ConfigUIResult res;
        if (configUI_.onMouseDown(x, y, w, h, profile_, selectedBtnIndex_, res)) {
            handleConfigUIResult(res);
            return;
        }
    }

    // 2. If waiting for a rebind (outside panel for LMB, or anywhere for RMB), bind the mouse button!
    if (rebindTarget_ != REBIND_NONE || configUI_.isWaitingSpecialRebind()) {
        if (onEditMouseBind(rightButton ? VK_RBUTTON : VK_LBUTTON)) {
            return;
        }
    }

    // 2. Right-click anywhere outside panel to drop a new button directly at cursor
    if (rightButton && !configUI_.containsPoint(x, y)) {
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
        showToast("New Key placed! Press ANY key or Mouse button (MMB/RMB/X1/X2/Wheel) to bind...", 3500);
        InvalidateRect(hwnd_, NULL, FALSE);
        return;
    }

    // 3. Hit-test canvas nodes (WASD, AIM, FIRE, L-FIRE, Floating Ball, Buttons, Swipe Ends)
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

void OverlayApp::onEditMouseMove(int x, int y) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    ConfigUIResult res;
    if (configUI_.onMouseMove(x, y, w, h, profile_, selectedBtnIndex_, res)) {
        handleConfigUIResult(res);
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
        case DRAG_JOY:
            profile_.joyX = nx;
            profile_.joyY = ny;
            break;
        case DRAG_AIM:
            profile_.aimX = nx;
            profile_.aimY = ny;
            break;
        case DRAG_FIRE:
            profile_.fireX = nx;
            profile_.fireY = ny;
            break;
        case DRAG_LEFT_FIRE:
            profile_.leftFireX = nx;
            profile_.leftFireY = ny;
            break;
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
        default:
            break;
    }
    InvalidateRect(hwnd_, NULL, FALSE);
}

void OverlayApp::onEditMouseUp() {
    configUI_.onMouseUp();
    if (dragging_ != DRAG_NONE) {
        dragging_ = DRAG_NONE;
        applyProfileToEngine();
    }
}

void OverlayApp::onEditMouseWheel(int x, int y, int delta) {
    if (configUI_.containsPoint(x, y)) return;

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

bool OverlayApp::onEditKeyDown(int vk) {
    // Let ConfigPanelUI capture typing or special hotkey rebinds first!
    ConfigUIResult res;
    if (configUI_.onKeyDown(vk, profile_, selectedBtnIndex_, res)) {
        handleConfigUIResult(res);
        return true;
    }

    if (rebindTarget_ != REBIND_NONE) {
        if (vk == VK_ESCAPE) {
            rebindTarget_ = REBIND_NONE;
            showToast("Rebind cancelled", 1000);
            InvalidateRect(hwnd_, NULL, FALSE);
            return true;
        }
        vk = normalizeKeyVk(vk);

        if (rebindTarget_ == REBIND_FIRE_KEY) {
            profile_.fireVk = vk;
            rebindTarget_ = REBIND_NONE;
            applyProfileToEngine();
            showToast("Fire key replaced -> " + vkToName(vk), 1800);
            InvalidateRect(hwnd_, NULL, FALSE);
            return true;
        }
        if (rebindTarget_ == REBIND_TOGGLE_AIM) {
            profile_.toggleAimVk = vk;
            rebindTarget_ = REBIND_NONE;
            applyProfileToEngine();
            showToast("Aim Lock/Unlock key replaced -> " + vkToName(vk), 1800);
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
                profile_.buttons[selectedBtnIndex_].label = comboToName(comboPrefix, vk);
            }
            rebindTarget_ = REBIND_NONE;
            applyProfileToEngine();
            showToast("Replaced '" + profile_.buttons[selectedBtnIndex_].label + "' -> " + comboToName(comboPrefix, vk), 1800);
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
        showToast("Node '" + b.label + "' -> " + actionTypeToName(b.action), 1800);
        InvalidateRect(hwnd_, NULL, FALSE);
        return true;
    }
    if (vk == VK_RETURN && selectedBtnIndex_ >= 0) {
        rebindTarget_ = REBIND_BUTTON;
        showToast("Replace '" + profile_.buttons[selectedBtnIndex_].label + "': Press ANY key or Mouse button (LMB/RMB/MMB/X1/X2/Wheel)...", 4000);
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

void OverlayApp::onPaint(HDC hdc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    // Reuse persistent offscreen backbuffer (only reallocate if mirror window resized!)
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
    if (demoMode_) {
        bgColor = RGB(14, 18, 26);
    } else if (editMode_) {
        bgColor = RGB(12, 16, 24);
    }
    HBRUSH bgBrush = CreateSolidBrush(bgColor);
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    if (demoMode_) {
        paintDemoBackdrop(memDC, w, h);
    }

    if (editMode_) {
        paintKeymapNodes(memDC, w, h, true);
        paintFloatingBall(memDC, w, h, true);
        configUI_.paint(memDC, w, h, profile_, profilePath_,
                        selectedBtnIndex_, (rebindTarget_ == REBIND_BUTTON),
                        fontSmall_, fontBold_, fontTitle_);
        if (rebindTarget_ != REBIND_NONE || configUI_.isWaitingSpecialRebind()) {
            paintRebindPrompt(memDC, w, h);
        }
    } else {
        if (profile_.showHudInPlay || demoMode_) {
            paintKeymapNodes(memDC, w, h, false);
        }
        paintFloatingBall(memDC, w, h, false);
        if (aimMode_ && !demoMode_) {
            int cx = w / 2, cy = h / 2;
            RECT dot = { cx - 1, cy - 1, cx + 2, cy + 2 };
            HBRUSH dotBrush = CreateSolidBrush(RGB(0, 255, 160));
            FillRect(memDC, &dot, dotBrush);
            DeleteObject(dotBrush);
        }
        paintPlayStatusBar(memDC, w);
    }

    // Toast notification banner at bottom-center
    if (!toastMsg_.empty() && GetTickCount() < toastExpireTick_) {
        SelectObject(memDC, fontBold_);
        SIZE sz;
        GetTextExtentPoint32A(memDC, toastMsg_.c_str(), static_cast<int>(toastMsg_.size()), &sz);
        int bw = sz.cx + 28;
        int bh = 30;
        RECT trc = { (w - bw) / 2, h - bh - 16, (w + bw) / 2, h - 16 };
        drawRoundedBox(memDC, trc, 10, RGB(18, 24, 36), RGB(0, 215, 255), 2);
        drawCenteredText(memDC, toastMsg_, w / 2, (trc.top + trc.bottom) / 2, RGB(240, 248, 255));
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
}

void OverlayApp::paintRebindPrompt(HDC hdc, int w, int h) {
    std::string targetName = "KEY";
    bool specialAimKey = false;
    if (configUI_.isRebindingToggleAim() || rebindTarget_ == REBIND_TOGGLE_AIM) {
        targetName = "AIM LOCK / UNLOCK";
        specialAimKey = true;
    } else if (configUI_.isRebindingHoldUnlock()) {
        targetName = "HOLD-TO-UNLOCK CURSOR";
        specialAimKey = true;
    } else if (rebindTarget_ == REBIND_FIRE_KEY) {
        targetName = "FIRE";
    } else if (rebindTarget_ == REBIND_BUTTON &&
               selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(profile_.buttons.size())) {
        targetName = profile_.buttons[selectedBtnIndex_].label;
    }

    const int bw = 576;
    const int bh = 70;
    int topY = h - bh - 56; // Sits cleanly below Studio Panel and above bottom toast
    if (topY < 14) topY = 14;
    RECT rc = { (w - bw) / 2, topY, (w + bw) / 2, topY + bh };
    drawRoundedBox(hdc, rc, 12, RGB(18, 26, 42), RGB(255, 210, 60), 3);

    SelectObject(hdc, fontTitle_);
    drawCenteredText(hdc, "REPLACE BUTTON: " + targetName, w / 2, rc.top + 22, RGB(255, 220, 80));

    SelectObject(hdc, fontSmall_);
    const char* hint = specialAimKey
        ? "Press ANY Keyboard Key (e.g. LShift, ~) or Mouse Button (MMB / RMB / X1 / X2 / Wheel)  |  ESC = Cancel"
        : "Press ANY Keyboard Key, Mouse Button (LMB / RMB / MMB / X1 / X2), Wheel, or Combo  |  ESC = Cancel";
    drawCenteredText(hdc, hint, w / 2, rc.top + 48, RGB(215, 232, 248));
}

void OverlayApp::paintDemoBackdrop(HDC hdc, int w, int h) {
    HPEN gridPen = CreatePen(PS_SOLID, 1, RGB(26, 33, 46));
    HGDIOBJ oldPen = SelectObject(hdc, gridPen);
    for (int x = 64; x < w; x += 64) {
        MoveToEx(hdc, x, 0, NULL);
        LineTo(hdc, x, h);
    }
    for (int y = 64; y < h; y += 64) {
        MoveToEx(hdc, 0, y, NULL);
        LineTo(hdc, w, y);
    }
    SelectObject(hdc, oldPen);
    DeleteObject(gridPen);

    int cx = w / 2, cy = h / 2;
    HPEN chPen = CreatePen(PS_SOLID, 1, RGB(60, 82, 104));
    oldPen = SelectObject(hdc, chPen);
    MoveToEx(hdc, cx - 14, cy, NULL); LineTo(hdc, cx + 14, cy);
    MoveToEx(hdc, cx, cy - 14, NULL); LineTo(hdc, cx, cy + 14);
    SelectObject(hdc, oldPen);
    DeleteObject(chPen);

    SelectObject(hdc, fontSmall_);
    drawCenteredText(hdc, "OFFLINE KEYMAP STUDIO CANVAS  (Click 'CFG' ball or F2 to open Studio | ~ to test Aim)",
                     w / 2, h - 54, RGB(115, 138, 165));
}

void OverlayApp::paintFloatingBall(HDC hdc, int w, int h, bool editMode) {
    int bx = static_cast<int>(profile_.floatBtnX * static_cast<float>(w));
    int by = static_cast<int>(profile_.floatBtnY * static_cast<float>(h));
    COLORREF fill = editMode ? RGB(0, 125, 170) : RGB(20, 28, 42);
    COLORREF ring = editMode ? RGB(255, 255, 255) : RGB(0, 225, 255);
    drawCircle(hdc, bx, by, FLOAT_BALL_RADIUS, fill, ring, 2, false);
    SelectObject(hdc, fontBold_);
    drawCenteredText(hdc, "CFG", bx, by, RGB(235, 250, 255));
}

void OverlayApp::paintPlayStatusBar(HDC hdc, int w) {
    EngineTelemetry tel = engine_->getTelemetry();
    const char* fmode = (profile_.fireMode == FIRE_MODE_DRAG_RIGHT) ? "DRAG-FIRE"
                      : (profile_.fireMode == FIRE_MODE_CLAW_LEFT)  ? "CLAW-LEFT"
                                                                    : "DUAL-DRAG";
    const char* stateTag = tel.holdViewActive ? "FREELOOK"
                         : (tel.aimMode && tel.scopeActive) ? "AIM+ADS"
                         : tel.aimMode ? "AIM ON" : "CURSOR";
    char hzBuf[16];
    if (profile_.pollHz <= 0) std::snprintf(hzBuf, sizeof(hzBuf), "MAX");
    else std::snprintf(hzBuf, sizeof(hzBuf), "%dHz", profile_.pollHz);

    std::string aimKeyName = vkToName(profile_.toggleAimVk);
    char text[300];
    std::snprintf(text, sizeof(text),
                  " %s | %s | %s | X:%.2f Y:%.2f | FX:%.2f FY:%.2f | %s (%u/s) | %s:Aim  F2/CFG:Studio  F4:HUD ",
                  stateTag, fmode, profile_.name.c_str(),
                  profile_.sensX, profile_.sensY,
                  profile_.fireSensX, profile_.fireSensY,
                  hzBuf, tel.touchRateHz, aimKeyName.c_str());

    SelectObject(hdc, fontBold_);
    SIZE sz;
    GetTextExtentPoint32A(hdc, text, static_cast<int>(std::strlen(text)), &sz);
    int bw = sz.cx + 16;
    int bh = 24;
    RECT bar = { (w - bw) / 2, 6, (w + bw) / 2, 6 + bh };
    COLORREF border = tel.holdViewActive ? RGB(0, 240, 255)
                    : (tel.aimMode && tel.scopeActive) ? RGB(170, 140, 255)
                    : tel.aimMode ? RGB(0, 230, 140) : RGB(255, 185, 40);
    drawRoundedBox(hdc, bar, 8, RGB(18, 22, 32), border, 2);
    drawCenteredText(hdc, text, w / 2, (bar.top + bar.bottom) / 2,
                     tel.aimMode ? RGB(185, 255, 220) : RGB(255, 225, 140));
}

void OverlayApp::paintKeymapNodes(HDC hdc, int w, int h, bool editMode) {
    EngineTelemetry tel = engine_->getTelemetry();

    // 1. WASD Virtual Joystick
    {
        int jx = static_cast<int>(profile_.joyX * w);
        int jy = static_cast<int>(profile_.joyY * h);
        int jr = static_cast<int>(profile_.joyRadius * h);
        int jsr = static_cast<int>(profile_.joySprintRadius * h);

        if (editMode) {
            HPEN dashPen = CreatePen(PS_DASH, 1, RGB(60, 200, 220));
            HGDIOBJ op = SelectObject(hdc, dashPen);
            HGDIOBJ ob = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Ellipse(hdc, jx - jsr, jy - jsr, jx + jsr, jy + jsr);
            SelectObject(hdc, op);
            SelectObject(hdc, ob);
            DeleteObject(dashPen);
        }

        drawCircle(hdc, jx, jy, jr, RGB(20, 40, 32), RGB(60, 235, 145), editMode ? 2 : 1, !editMode);
        SelectObject(hdc, fontBold_);
        drawCenteredText(hdc, "W", jx, jy - jr + 12, RGB(140, 255, 190));
        drawCenteredText(hdc, "S", jx, jy + jr - 12, RGB(140, 255, 190));
        drawCenteredText(hdc, "A", jx - jr + 12, jy, RGB(140, 255, 190));
        drawCenteredText(hdc, "D", jx + jr - 12, jy, RGB(140, 255, 190));

        if (tel.joyDown) {
            int tx = static_cast<int>(tel.joyFingerNormX * w);
            int ty = static_cast<int>(tel.joyFingerNormY * h);
            drawCircle(hdc, tx, ty, 8, RGB(60, 255, 150), RGB(255, 255, 255), 2);
        }
    }

    // 2. Aim Anchor & Safe Box
    {
        int ax = static_cast<int>(profile_.aimX * w);
        int ay = static_cast<int>(profile_.aimY * h);
        int hw = static_cast<int>(profile_.aimHalfW * w);
        int hh = static_cast<int>(profile_.aimHalfH * h);

        if ((editMode && !profile_.unboundedAim) || demoMode_) {
            RECT box = {
                (ax - hw < w / 3) ? (w / 3) : (ax - hw),
                (ay - hh < 30) ? 30 : (ay - hh),
                (ax + hw > w - 6) ? (w - 6) : (ax + hw),
                (ay + hh > h - 6) ? (h - 6) : (ay + hh)
            };
            drawRoundedBox(hdc, box, 8, RGB(0, 0, 0), RGB(255, 195, 45), editMode ? 2 : 1, true);
        }

        drawCircle(hdc, ax, ay, editMode ? 22 : 14, RGB(45, 36, 16), RGB(255, 200, 50), 2, !editMode);
        SelectObject(hdc, fontBold_);
        drawCenteredText(hdc, "AIM", ax, ay, RGB(255, 220, 100));

        if (tel.aimFingerDown) {
            int fx = static_cast<int>(tel.aimFingerNormX * w);
            int fy = static_cast<int>(tel.aimFingerNormY * h);
            COLORREF c = tel.fireHeld ? RGB(255, 70, 70) : RGB(255, 215, 0);
            drawCircle(hdc, fx, fy, 7, c, RGB(255, 255, 255), 2);
        }
    }

    // 3. Fire Buttons (Right Fire & Left Fire)
    {
        bool usesRightFire = (profile_.fireMode != FIRE_MODE_CLAW_LEFT);
        int fx = static_cast<int>(profile_.fireX * w);
        int fy = static_cast<int>(profile_.fireY * h);
        if (usesRightFire || editMode) {
            COLORREF border = usesRightFire ? RGB(255, 80, 60) : RGB(130, 75, 75);
            drawCircle(hdc, fx, fy, 24, RGB(58, 22, 22), border, 2, !editMode);
            SelectObject(hdc, fontBold_);
            drawCenteredText(hdc, vkToName(profile_.fireVk), fx, fy - 5, RGB(255, 170, 150));
            SelectObject(hdc, fontSmall_);
            drawCenteredText(hdc, "FIRE", fx, fy + 9, RGB(255, 120, 100));
        }

        if (!usesRightFire || editMode) {
            int lfx = static_cast<int>(profile_.leftFireX * w);
            int lfy = static_cast<int>(profile_.leftFireY * h);
            COLORREF border = (!usesRightFire) ? RGB(255, 80, 60) : RGB(130, 75, 75);
            drawCircle(hdc, lfx, lfy, 20, RGB(52, 22, 28), border, 2, !editMode);
            SelectObject(hdc, fontSmall_);
            drawCenteredText(hdc, "L-FIRE", lfx, lfy, RGB(255, 175, 175));
        }
    }

    // 4. Action Nodes
    for (size_t i = 0; i < profile_.buttons.size(); ++i) {
        const KeyNode& b = profile_.buttons[i];
        int bx = static_cast<int>(b.x * w);
        int by = static_cast<int>(b.y * h);
        int br = static_cast<int>(b.radius * h);
        if (br < 18) br = 18;
        if (br > 28) br = 28;

        if (editMode && b.action == NODE_SWIPE) {
            int bx2 = static_cast<int>(b.x2 * w);
            int by2 = static_cast<int>(b.y2 * h);
            HPEN spPen = CreatePen(PS_SOLID, 2, RGB(255, 165, 60));
            HGDIOBJ op = SelectObject(hdc, spPen);
            MoveToEx(hdc, bx, by, NULL);
            LineTo(hdc, bx2, by2);
            SelectObject(hdc, op);
            DeleteObject(spPen);
            drawCircle(hdc, bx2, by2, 9, RGB(255, 165, 60), RGB(255, 255, 255), 2);
            SelectObject(hdc, fontSmall_);
            drawCenteredText(hdc, "END", bx2, by2 + 16, RGB(255, 195, 110));
        }

        bool selected = editMode && (static_cast<int>(i) == selectedBtnIndex_);
        COLORREF typeColor = RGB(80, 190, 255);
        if (b.action == NODE_DOWN_UP) typeColor = RGB(80, 235, 160);
        else if (b.action == NODE_HOLD_VIEW) typeColor = RGB(0, 240, 240);
        else if (b.action == NODE_SWIPE) typeColor = RGB(255, 165, 60);
        else if (b.action == NODE_CURSOR_TOGGLE) typeColor = RGB(195, 120, 255);
        else if (b.action == NODE_RESET_VIEW || b.action == NODE_SWITCH_SENS || b.action == NODE_REACTIVATE_JOY)
            typeColor = RGB(255, 210, 80);

        COLORREF border = selected ? RGB(255, 255, 255) : typeColor;
        COLORREF fill   = selected ? RGB(30, 72, 96) : RGB(24, 30, 44);

        drawCircle(hdc, bx, by, br, fill, border, selected ? 3 : 2, !editMode);

        SelectObject(hdc, fontBold_);
        std::string keyTxt;
        if (selected && rebindTarget_ == REBIND_BUTTON) {
            keyTxt = "?";
        } else if (b.comboVk == 0 && b.vk == VK_CUSTOM_MWHEEL_UP) {
            keyTxt = "SCRL+";
        } else if (b.comboVk == 0 && b.vk == VK_CUSTOM_MWHEEL_DOWN) {
            keyTxt = "SCRL-";
        } else {
            keyTxt = comboToName(b.comboVk, b.vk);
        }
        drawCenteredText(hdc, keyTxt, bx, by - 5, RGB(245, 250, 255));
        SelectObject(hdc, fontSmall_);
        drawCenteredText(hdc, b.label, bx, by + 9, RGB(170, 210, 240));
        if (editMode && b.action != NODE_PRESS && b.action != NODE_CLICK) {
            drawCenteredText(hdc, actionTypeToName(b.action), bx, by + br + 9, typeColor);
        }
    }
}
