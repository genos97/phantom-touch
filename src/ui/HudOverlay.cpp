#include "ui/HudOverlay.hpp"
#include "config/KeyCodes.hpp"

#include <cstdio>

namespace {

// Near-black color key prevents purple/magenta fringing around crisp HUD badges in Play Mode
static const COLORREF kTransparentKey = RGB(1, 2, 3);
static const COLORREF kEditWashColor  = RGB(10, 14, 22);

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

HudOverlay::HudOverlay(IHudOverlayHost* host) : host_(host) {
    ZeroMemory(&lastScreenRect_, sizeof(lastScreenRect_));
}

HudOverlay::~HudOverlay() {
    destroy();
}

void HudOverlay::destroy() {
    if (cacheDC_) {
        if (cacheOldBmp_) SelectObject(cacheDC_, cacheOldBmp_);
        if (cacheBmp_) DeleteObject(cacheBmp_);
        DeleteDC(cacheDC_);
        cacheDC_ = NULL;
        cacheBmp_ = NULL;
        cacheOldBmp_ = NULL;
    }
    if (fonts_.smallFont) { DeleteObject(fonts_.smallFont); fonts_.smallFont = NULL; }
    if (fonts_.boldFont)  { DeleteObject(fonts_.boldFont);  fonts_.boldFont = NULL; }
    if (fonts_.titleFont) { DeleteObject(fonts_.titleFont); fonts_.titleFont = NULL; }
    if (hwnd_ && IsWindow(hwnd_)) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = NULL;
}

bool HudOverlay::create(HWND ownerShellHwnd, HINSTANCE hInst) {
    ownerHwnd_ = ownerShellHwnd;
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXA wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = HudOverlay::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = "PhantomTouchHudOverlay";
        if (!RegisterClassExA(&wc)) return false;
        reg = true;
    }

    fonts_.smallFont = CreateFontA(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    fonts_.boldFont  = CreateFontA(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    fonts_.titleFont = CreateFontA(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");

    // Owned by ShellWindow (NO WS_EX_TOPMOST!): stays strictly above embedded scrcpy,
    // yet respects Alt-Tab cleanly when switching to other apps!
    DWORD exStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    hwnd_ = CreateWindowExA(exStyle, "PhantomTouchHudOverlay", "Phantom Touch HUD",
                            WS_POPUP, 0, 0, 100, 100,
                            ownerShellHwnd, NULL, hInst, this);
    if (!hwnd_) return false;
    applyOpacity(false, 0.75f);
    return true;
}

void HudOverlay::syncBounds(const RECT& videoScreenRect, bool visible) {
    if (!hwnd_) return;
    int w = videoScreenRect.right - videoScreenRect.left;
    int h = videoScreenRect.bottom - videoScreenRect.top;
    if (!visible || w < 32 || h < 32) {
        if (visible_) {
            ShowWindow(hwnd_, SW_HIDE);
            visible_ = false;
        }
        return;
    }

    bool moved = (videoScreenRect.left != lastScreenRect_.left ||
                  videoScreenRect.top != lastScreenRect_.top ||
                  videoScreenRect.right != lastScreenRect_.right ||
                  videoScreenRect.bottom != lastScreenRect_.bottom);
    if (moved || !visible_) {
        lastScreenRect_ = videoScreenRect;
        SetWindowPos(hwnd_, HWND_TOP,
                     videoScreenRect.left, videoScreenRect.top, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
        visible_ = true;
        invalidate();
    }
}

void HudOverlay::setInteractiveEdit(bool editMode, float hudOpacity) {
    if (!hwnd_) return;
    interactive_ = editMode;
    dragging_ = DRAG_NONE;
    if (!editMode) {
        rebindTarget_ = HUD_REBIND_NONE;
    }

    LONG ex = GetWindowLongA(hwnd_, GWL_EXSTYLE);
    if (editMode) {
        ex &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    } else {
        ex |= (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
    }
    SetWindowLongA(hwnd_, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd_, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    applyOpacity(editMode, hudOpacity);
    invalidate();
}

void HudOverlay::applyOpacity(bool editMode, float hudOpacity) {
    if (!hwnd_) return;
    if (editMode) {
        // Pure LWA_ALPHA (NO LWA_COLORKEY!) in Edit Mode so 100% of the phone canvas
        // captures clicks, drags, double-clicks, and right-clicks with zero holes!
        SetLayeredWindowAttributes(hwnd_, 0, 165, LWA_ALPHA);
    } else {
        BYTE alpha = static_cast<BYTE>(clampRange(hudOpacity, 0.15f, 1.00f) * 255.0f);
        SetLayeredWindowAttributes(hwnd_, kTransparentKey, alpha, LWA_COLORKEY | LWA_ALPHA);
    }
}

void HudOverlay::invalidate() {
    if (hwnd_ && visible_) {
        InvalidateRect(hwnd_, NULL, FALSE);
    }
}

void HudOverlay::setSelectedBtnIndex(int idx) {
    selectedBtnIndex_ = idx;
    invalidate();
}

void HudOverlay::startRebind(HudRebindTarget target) {
    rebindTarget_ = target;
    invalidate();
}

void HudOverlay::cancelRebind() {
    if (rebindTarget_ != HUD_REBIND_NONE) {
        rebindTarget_ = HUD_REBIND_NONE;
        invalidate();
    }
}

void HudOverlay::showToast(const std::string& msg, DWORD durationMs) {
    toastMsg_ = msg;
    toastExpireTick_ = GetTickCount() + durationMs;
    toastWasVisible_ = true;
    invalidate();
}

bool HudOverlay::checkToastExpired() {
    bool active = (!toastMsg_.empty() && GetTickCount() < toastExpireTick_);
    bool expired = (toastWasVisible_ && !active);
    toastWasVisible_ = active;
    if (expired) invalidate();
    return expired;
}

LRESULT CALLBACK HudOverlay::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HudOverlay* self = NULL;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = reinterpret_cast<CREATESTRUCTA*>(lParam);
        self = reinterpret_cast<HudOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<HudOverlay*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

LRESULT HudOverlay::handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            onPaint(hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (interactive_) {
                SetCapture(hwnd);
                onMouseDown(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), false);
                return 0;
            }
            break;
        case WM_LBUTTONDBLCLK:
            if (interactive_) {
                onDoubleClick(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                return 0;
            }
            break;
        case WM_RBUTTONDOWN:
            if (interactive_) {
                onMouseDown(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), true);
                return 0;
            }
            break;
        case WM_MBUTTONDOWN:
            if (interactive_ && handleMouseBind(VK_MBUTTON)) return 0;
            break;
        case WM_XBUTTONDOWN:
            if (interactive_) {
                int xvk = (HIWORD(wParam) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
                if (handleMouseBind(xvk)) return 0;
            }
            break;
        case WM_MOUSEMOVE:
            if (interactive_) {
                onMouseMove(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)));
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (interactive_) {
                ReleaseCapture();
                onMouseUp();
                return 0;
            }
            break;
        case WM_MOUSEWHEEL:
            if (interactive_) {
                int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                int wvk = (delta > 0) ? VK_CUSTOM_MWHEEL_UP : VK_CUSTOM_MWHEEL_DOWN;
                if (handleMouseBind(wvk)) return 0;
                POINT pt = { static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
                ScreenToClient(hwnd, &pt);
                onMouseWheel(pt.x, pt.y, delta);
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (interactive_) {
                if (handleKeyDown(static_cast<int>(wParam))) return 0;
            } else if (msg == WM_SYSKEYDOWN) {
                return 0;
            }
            break;
        case WM_SYSKEYUP:
        case WM_SYSCHAR:
            if (!interactive_) return 0;
            break;
        case WM_SYSCOMMAND:
            if (!interactive_ && (wParam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_SETCURSOR:
            if (host_ && host_->hudIsAimMode()) {
                SetCursor(NULL);
                return TRUE;
            }
            break;
        default:
            break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

HudOverlay::DragTarget HudOverlay::hitTestNode(int px, int py, int& outBtnIdx) const {
    outBtnIdx = -1;
    if (!host_ || !hwnd_) return DRAG_NONE;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return DRAG_NONE;

    const Profile& p = host_->hudProfile();
    for (int i = static_cast<int>(p.buttons.size()) - 1; i >= 0; --i) {
        if (p.buttons[i].action != NODE_SWIPE) continue;
        int ex = static_cast<int>(p.buttons[i].x2 * w);
        int ey = static_cast<int>(p.buttons[i].y2 * h);
        int dx = px - ex, dy = py - ey;
        if (dx * dx + dy * dy <= 14 * 14) { outBtnIdx = i; return DRAG_SWIPE_END; }
    }
    for (int i = static_cast<int>(p.buttons.size()) - 1; i >= 0; --i) {
        int bx = static_cast<int>(p.buttons[i].x * w);
        int by = static_cast<int>(p.buttons[i].y * h);
        int br = static_cast<int>(p.buttons[i].radius * h);
        if (br < 18) br = 18;
        int dx = px - bx, dy = py - by;
        if (dx * dx + dy * dy <= br * br) { outBtnIdx = i; return DRAG_BUTTON; }
    }
    {
        int fx = static_cast<int>(p.fireX * w), fy = static_cast<int>(p.fireY * h);
        if ((px - fx) * (px - fx) + (py - fy) * (py - fy) <= 28 * 28) return DRAG_FIRE;
    }
    {
        int lfx = static_cast<int>(p.leftFireX * w), lfy = static_cast<int>(p.leftFireY * h);
        if ((px - lfx) * (px - lfx) + (py - lfy) * (py - lfy) <= 24 * 24) return DRAG_LEFT_FIRE;
    }
    {
        int ax = static_cast<int>(p.aimX * w), ay = static_cast<int>(p.aimY * h);
        if ((px - ax) * (px - ax) + (py - ay) * (py - ay) <= 30 * 30) return DRAG_AIM;
    }
    {
        int jx = static_cast<int>(p.joyX * w), jy = static_cast<int>(p.joyY * h);
        int jr = static_cast<int>(p.joyRadius * h);
        if (jr < 28) jr = 28;
        if ((px - jx) * (px - jx) + (py - jy) * (py - jy) <= jr * jr) return DRAG_JOY;
    }
    return DRAG_NONE;
}

bool HudOverlay::handleMouseBind(int mouseVk) {
    if (!host_) return false;
    if (host_->hudIsSpecialDrawerRebind()) {
        host_->hudOnMouseBindToDrawer(mouseVk);
        return true;
    }
    if (rebindTarget_ == HUD_REBIND_NONE) return false;

    Profile& p = host_->hudProfile();
    if (rebindTarget_ == HUD_REBIND_BUTTON &&
        selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        p.buttons[selectedBtnIndex_].comboVk = 0;
        p.buttons[selectedBtnIndex_].vk = mouseVk;
        if (p.buttons[selectedBtnIndex_].label == "KEY") {
            p.buttons[selectedBtnIndex_].label = KeyCodes::toName(mouseVk);
        }
        rebindTarget_ = HUD_REBIND_NONE;
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Replaced '" + p.buttons[selectedBtnIndex_].label + "' -> " + KeyCodes::toName(mouseVk), 1800);
        invalidate();
        return true;
    }
    if (rebindTarget_ == HUD_REBIND_FIRE_KEY) {
        p.fireVk = mouseVk;
        rebindTarget_ = HUD_REBIND_NONE;
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Fire key -> " + KeyCodes::toName(mouseVk), 1800);
        invalidate();
        return true;
    }
    if (rebindTarget_ == HUD_REBIND_TOGGLE_AIM) {
        if (mouseVk == VK_LBUTTON) {
            host_->hudOnToast("Use Keyboard, MMB, RMB, X1, X2, or Wheel for Aim Lock", 2400);
            return true;
        }
        p.toggleAimVk = mouseVk;
        rebindTarget_ = HUD_REBIND_NONE;
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Aim Lock/Unlock key -> " + KeyCodes::toName(mouseVk), 1800);
        invalidate();
        return true;
    }
    return false;
}

void HudOverlay::onDoubleClick(int x, int y) {
    if (!host_) return;
    Profile& p = host_->hudProfile();
    int btnIdx = -1;
    DragTarget t = hitTestNode(x, y, btnIdx);
    if (t == DRAG_BUTTON && btnIdx >= 0 && btnIdx < static_cast<int>(p.buttons.size())) {
        selectedBtnIndex_ = btnIdx;
        dragging_ = DRAG_NONE;
        rebindTarget_ = HUD_REBIND_BUTTON;
        host_->hudOnSelectionChanged(btnIdx);
        host_->hudOnToast("Replace '" + p.buttons[btnIdx].label + "': Press ANY key, Mouse button, or Scroll", 4000);
        invalidate();
    } else if (t == DRAG_FIRE || t == DRAG_LEFT_FIRE) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = HUD_REBIND_FIRE_KEY;
        host_->hudOnToast("Replace FIRE key: Press ANY key or Mouse button", 4000);
        invalidate();
    } else if (t == DRAG_AIM) {
        dragging_ = DRAG_NONE;
        rebindTarget_ = HUD_REBIND_TOGGLE_AIM;
        host_->hudOnToast("Replace AIM LOCK key: Press ANY key (LShift, ~) or MMB", 4000);
        invalidate();
    }
}

void HudOverlay::onMouseDown(int x, int y, bool rightBtn) {
    if (!host_ || !hwnd_) return;
    if (rebindTarget_ != HUD_REBIND_NONE || host_->hudIsSpecialDrawerRebind()) {
        if (handleMouseBind(rightBtn ? VK_RBUTTON : VK_LBUTTON)) return;
    }

    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    Profile& p = host_->hudProfile();
    if (rightBtn) {
        KeyNode n;
        n.label = "KEY";
        n.vk = 'K';
        n.action = NODE_PRESS;
        n.x = clamp01(static_cast<float>(x) / static_cast<float>(w));
        n.y = clamp01(static_cast<float>(y) / static_cast<float>(h));
        n.x2 = n.x;
        n.y2 = n.y;
        n.radius = 0.030f;
        p.buttons.push_back(n);
        selectedBtnIndex_ = static_cast<int>(p.buttons.size()) - 1;
        rebindTarget_ = HUD_REBIND_BUTTON;
        host_->hudOnSelectionChanged(selectedBtnIndex_);
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("New Key placed! Press ANY key, Mouse button, or Scroll...", 3500);
        invalidate();
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
    host_->hudOnSelectionChanged(selectedBtnIndex_);
    invalidate();
}

void HudOverlay::onMouseMove(int x, int y) {
    if (!host_ || !hwnd_ || dragging_ == DRAG_NONE) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    float nx = clamp01(static_cast<float>(x) / static_cast<float>(w));
    float ny = clamp01(static_cast<float>(y) / static_cast<float>(h));
    Profile& p = host_->hudProfile();

    switch (dragging_) {
        case DRAG_JOY:       p.joyX = nx; p.joyY = ny; break;
        case DRAG_AIM:       p.aimX = nx; p.aimY = ny; break;
        case DRAG_FIRE:      p.fireX = nx; p.fireY = ny; break;
        case DRAG_LEFT_FIRE: p.leftFireX = nx; p.leftFireY = ny; break;
        case DRAG_BUTTON:
            if (dragBtnIndex_ >= 0 && dragBtnIndex_ < static_cast<int>(p.buttons.size())) {
                KeyNode& b = p.buttons[dragBtnIndex_];
                float dx = nx - b.x, dy = ny - b.y;
                b.x = nx; b.y = ny;
                if (b.action == NODE_SWIPE) {
                    b.x2 = clamp01(b.x2 + dx);
                    b.y2 = clamp01(b.y2 + dy);
                }
            }
            break;
        case DRAG_SWIPE_END:
            if (dragBtnIndex_ >= 0 && dragBtnIndex_ < static_cast<int>(p.buttons.size())) {
                p.buttons[dragBtnIndex_].x2 = nx;
                p.buttons[dragBtnIndex_].y2 = ny;
            }
            break;
        default: break;
    }
    host_->hudOnProfileChanged(false);
    invalidate();
}

void HudOverlay::onMouseUp() {
    if (dragging_ != DRAG_NONE) {
        dragging_ = DRAG_NONE;
        if (host_) host_->hudOnProfileChanged(true);
    }
}

void HudOverlay::onMouseWheel(int x, int y, int delta) {
    if (!host_) return;
    int btnIdx = -1;
    DragTarget t = hitTestNode(x, y, btnIdx);
    float step = (delta > 0) ? 0.010f : -0.010f;
    bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    Profile& p = host_->hudProfile();

    if (t == DRAG_JOY) {
        if (shiftHeld) p.joySprintRadius = clampRange(p.joySprintRadius + step, 0.04f, 0.35f);
        else p.joyRadius = clampRange(p.joyRadius + step, 0.03f, 0.25f);
    } else if (t == DRAG_AIM) {
        if (shiftHeld) p.aimHalfH = clampRange(p.aimHalfH + step, 0.06f, 0.80f);
        else {
            p.aimHalfW = clampRange(p.aimHalfW + step, 0.06f, 0.45f);
            p.aimHalfH = clampRange(p.aimHalfH + step, 0.06f, 0.80f);
        }
    } else if (t == DRAG_FIRE) {
        p.fireBoxHalfH = clampRange(p.fireBoxHalfH + step, 0.10f, 0.90f);
    } else {
        return;
    }
    host_->hudOnProfileChanged(true);
    invalidate();
}

bool HudOverlay::handleKeyDown(int vk) {
    if (!host_) return false;
    Profile& p = host_->hudProfile();

    if (rebindTarget_ != HUD_REBIND_NONE) {
        if (vk == VK_ESCAPE) {
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnToast("Rebind cancelled", 1000);
            invalidate();
            return true;
        }
        vk = KeyCodes::normalizeModifier(vk);
        if (rebindTarget_ == HUD_REBIND_FIRE_KEY) {
            p.fireVk = vk;
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnProfileChanged(true);
            host_->hudOnToast("Fire key -> " + KeyCodes::toName(vk), 1800);
            invalidate();
            return true;
        }
        if (rebindTarget_ == HUD_REBIND_TOGGLE_AIM) {
            p.toggleAimVk = vk;
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnProfileChanged(true);
            host_->hudOnToast("Aim Lock/Unlock key -> " + KeyCodes::toName(vk), 1800);
            invalidate();
            return true;
        }
        if (rebindTarget_ == HUD_REBIND_BUTTON &&
            selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
            int comboPrefix = 0;
            if (vk != VK_LMENU && vk != VK_RMENU && (GetAsyncKeyState(VK_LMENU) & 0x8000)) comboPrefix = VK_LMENU;
            else if (vk != VK_LCONTROL && vk != VK_RCONTROL && (GetAsyncKeyState(VK_LCONTROL) & 0x8000)) comboPrefix = VK_LCONTROL;
            else if (vk != VK_LSHIFT && vk != VK_RSHIFT && (GetAsyncKeyState(VK_LSHIFT) & 0x8000)) comboPrefix = VK_LSHIFT;

            p.buttons[selectedBtnIndex_].comboVk = comboPrefix;
            p.buttons[selectedBtnIndex_].vk = vk;
            if (p.buttons[selectedBtnIndex_].label == "KEY") {
                p.buttons[selectedBtnIndex_].label = KeyCodes::comboToName(comboPrefix, vk);
            }
            rebindTarget_ = HUD_REBIND_NONE;
            host_->hudOnProfileChanged(true);
            host_->hudOnToast("Replaced '" + p.buttons[selectedBtnIndex_].label + "' -> " + KeyCodes::comboToName(comboPrefix, vk), 1800);
            invalidate();
        }
        return true;
    }

    bool ctrlHeld = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrlHeld && vk == 'S') {
        host_->hudOnRequestSave();
        return true;
    }
    if (vk == VK_F2 || vk == VK_ESCAPE) {
        host_->hudOnRequestCloseEdit();
        return true;
    }
    if (vk == 'T' && selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        KeyNode& b = p.buttons[selectedBtnIndex_];
        b.action = static_cast<KeyActionType>((static_cast<int>(b.action) + 1) % 10);
        if (b.action == NODE_SWIPE && b.x2 == b.x && b.y2 == b.y) {
            b.x2 = clamp01(b.x + 0.12f);
            b.durationMs = 120;
        }
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Node '" + b.label + "' -> " + ProfileDefaults::actionTypeToName(b.action), 1600);
        invalidate();
        return true;
    }
    if (vk == VK_RETURN && selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        rebindTarget_ = HUD_REBIND_BUTTON;
        host_->hudOnToast("Replace '" + p.buttons[selectedBtnIndex_].label + "': Press ANY key, Mouse button, or Scroll", 4000);
        invalidate();
        return true;
    }
    if (vk == VK_DELETE && selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
        p.buttons.erase(p.buttons.begin() + selectedBtnIndex_);
        selectedBtnIndex_ = -1;
        host_->hudOnSelectionChanged(-1);
        host_->hudOnProfileChanged(true);
        host_->hudOnToast("Key deleted", 1200);
        invalidate();
        return true;
    }
    return false;
}

void HudOverlay::onPaint(HDC hdc) {
    if (!host_ || !hwnd_) return;
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

    const Profile& p = host_->hudProfile();
    EngineTelemetry tel = host_->hudTelemetry();
    bool editMode = host_->hudIsEditMode();
    bool demoMode = host_->hudIsDemoMode();
    bool aimMode  = host_->hudIsAimMode();

    HBRUSH bgBrush = CreateSolidBrush(editMode ? kEditWashColor : kTransparentKey);
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    if (editMode) {
        // Subtle cyan outline around phone viewport during Keymap Edit Mode (live game stays 100% clear!)
        HPEN borderPen = CreatePen(PS_SOLID, 2, RGB(0, 212, 255));
        HGDIOBJ op = SelectObject(memDC, borderPen);
        HGDIOBJ ob = SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, 1, 1, w - 1, h - 1);
        SelectObject(memDC, op);
        SelectObject(memDC, ob);
        DeleteObject(borderPen);

        HudRenderer::paintKeymapNodes(memDC, w, h, p, tel, true, demoMode,
                                      selectedBtnIndex_, (rebindTarget_ == HUD_REBIND_BUTTON), fonts_);

        if (rebindTarget_ != HUD_REBIND_NONE || host_->hudIsSpecialDrawerRebind()) {
            std::string targetName = "KEY";
            bool specialAim = false;
            if (host_->hudIsSpecialDrawerRebind()) {
                targetName = host_->hudSpecialRebindLabel();
                specialAim = host_->hudIsSpecialAimRebind();
            } else if (rebindTarget_ == HUD_REBIND_TOGGLE_AIM) {
                targetName = "AIM LOCK / UNLOCK";
                specialAim = true;
            } else if (rebindTarget_ == HUD_REBIND_FIRE_KEY) {
                targetName = "FIRE";
            } else if (rebindTarget_ == HUD_REBIND_BUTTON &&
                       selectedBtnIndex_ >= 0 && selectedBtnIndex_ < static_cast<int>(p.buttons.size())) {
                targetName = p.buttons[selectedBtnIndex_].label;
            }
            HudRenderer::paintRebindPrompt(memDC, w, h, targetName, specialAim, fonts_);
        }
    } else {
        if (p.showHudInPlay || demoMode) {
            HudRenderer::paintKeymapNodes(memDC, w, h, p, tel, false, demoMode, -1, false, fonts_);
        }
        if (aimMode && !demoMode) {
            int cx = w / 2, cy = h / 2;
            RECT dot = { cx - 1, cy - 1, cx + 2, cy + 2 };
            HBRUSH dotBrush = CreateSolidBrush(RGB(0, 255, 160));
            FillRect(memDC, &dot, dotBrush);
            DeleteObject(dotBrush);
        }
        if (host_->hudIsFullscreen() && !aimMode) {
            HudRenderer::paintPlayStatusBar(memDC, w, p, tel, fonts_);
        }
    }

    if (!toastMsg_.empty() && GetTickCount() < toastExpireTick_) {
        HudRenderer::paintToast(memDC, w, h, toastMsg_, fonts_);
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
}
